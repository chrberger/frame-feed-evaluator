/*
 * Copyright (C) 2019  Christian Berger
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "cluon-complete.hpp"
#include "opendlv-standard-message-set.hpp"

#include "lodepng.h"

#include <wels/codec_api.h>
#include <libyuv.h>
#include <X11/Xlib.h>

#include <cstdint>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

int32_t main(int32_t argc, char **argv) {
  int32_t retCode{1};
  auto commandlineArguments = cluon::getCommandlineArguments(argc, argv);
  if ( (0 == commandlineArguments.count("folder")) ||
       (0 == commandlineArguments.count("name")) ||
       (0 == commandlineArguments.count("cid")) ) {
    std::cerr << argv[0] << " 'replays' a sequence of *.png files into i420 frames and waits for an ImageReading response before next frame." << std::endl;
    std::cerr << "Usage:   " << argv[0] << " --folder=<Folder with *.png files to replay> [--verbose]" << std::endl;
    std::cerr << "         --folder:      path to a folder with .png files" << std::endl;
    std::cerr << "         --name:        name of the shared memory area to create for i420 frame" << std::endl;
    std::cerr << "         --cid:         CID of the OD4Session to listen for encoded h264 frames" << std::endl;
    std::cerr << "         --delay:       delay between frames in ms; default: 1000" << std::endl;
    std::cerr << "         --delay.start: delay before the first frame is replayed in ms; default: 5000" << std::endl;
    std::cerr << "         --verbose:     display PNG frame while replaying" << std::endl;
    std::cerr << "Example: " << argv[0] << " --folder=. --verbose" << std::endl;
    retCode = 1;
  } else {
    const std::string folderWithPNGs{commandlineArguments["folder"]};
    const std::string NAME{commandlineArguments["name"]};
    const uint32_t DELAY_START{(commandlineArguments["delay.start"].size() != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["delay.start"])) : 5000};
    const uint32_t DELAY{(commandlineArguments["delay"].size() != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["delay"])) : 1000};
    const bool VERBOSE{commandlineArguments.count("verbose") != 0};

    // Show frames.
    Display *display{nullptr};
    Visual *visual{nullptr};
    Window window{0};
    XImage *ximage{nullptr};

    // openh264 openh264Decoder.
    ISVCDecoder *openh264Decoder{nullptr};
    WelsCreateDecoder(&openh264Decoder);
    if (0 != WelsCreateDecoder(&openh264Decoder) && (nullptr != openh264Decoder)) {
      std::cerr << "[frame-feed-evaluator]: : Failed to create openh264 openh264Decoder." << std::endl;
      return retCode;
    }

    int logLevel{VERBOSE ? WELS_LOG_INFO : WELS_LOG_QUIET};
    openh264Decoder->SetOption(DECODER_OPTION_TRACE_LEVEL, &logLevel);

    SDecodingParam decodingParam;
    {
      memset(&decodingParam, 0, sizeof(SDecodingParam));
      decodingParam.eEcActiveIdc = ERROR_CON_DISABLE;
      decodingParam.bParseOnly = false;
      decodingParam.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;
    }
    if (cmResultSuccess != openh264Decoder->Initialize(&decodingParam)) {
      std::cerr << "[frame-feed-evaluator]: : Failed to initialize openh264 openh264Decoder." << std::endl;
      return retCode;
    }

    // Frame data.
    std::vector<unsigned char> rawRGBAFromPNG;
    std::vector<unsigned char> rawABGRFrame;
    std::unique_ptr<cluon::SharedMemory> sharedMemoryFori420{nullptr};

    cluon::OD4Session od4{static_cast<uint16_t>(std::stoi(commandlineArguments["cid"]))};
    if (od4.isRunning()) {
      std::atomic<bool> hasReceivedImageReading{false};
      opendlv::proxy::ImageReading imageReading;
      od4.dataTrigger(opendlv::proxy::ImageReading::ID(), [&hasReceivedImageReading, &imageReading](cluon::data::Envelope &&env){
        if (opendlv::proxy::ImageReading::ID() == env.dataType()) {
          imageReading = cluon::extractMessage<opendlv::proxy::ImageReading>(std::move(env));
          hasReceivedImageReading.store(true);
        }
      });

      cluon::data::TimeStamp before, after;
      uint32_t width{0}, height{0};
      for (const auto &entry : std::filesystem::directory_iterator(folderWithPNGs)) {
        std::string filename{entry.path()};
        if (std::string::npos != filename.find(".png")) {
          if (VERBOSE) {
            std::clog << "[frame-feed-evaluator]: Processing '" << filename << "'." << std::endl;
          }

          // Reset raw buffer for PNG.
          rawRGBAFromPNG.clear();
          unsigned lodePNGRetVal = lodepng::decode(rawRGBAFromPNG, width, height, filename.c_str());
          if (0 == lodePNGRetVal) {
            rawABGRFrame.reserve(rawRGBAFromPNG.size());

            // Initialize output frame in i420 format.
            if (!sharedMemoryFori420) {
              sharedMemoryFori420.reset(new cluon::SharedMemory{NAME, width * height * 3/2});
              std::clog << "[frame-feed-evaluator]: Created shared memory '" << NAME << "' of size " << sharedMemoryFori420->size() << " holding an i420 frame of size " << width << "x" << height << "." << std::endl;

              // Once the shared memory is created, wait for the first frame to replay
              // so that any downstream processes can attach to it.
              if (0 < DELAY_START) {
                std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(DELAY_START));
              }
            }

            // Exclusive access to shared memory.
            sharedMemoryFori420->lock();
            {
              libyuv::ABGRToI420(reinterpret_cast<uint8_t*>(rawRGBAFromPNG.data()), width * 4 /* 4*WIDTH for ABGR*/,
                                 reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()), width,
                                 reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(width * height)), width/2,
                                 reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(width * height + ((width * height) >> 2))), width/2,
                                 width, height);

              // When we need to show the image, transform from i420 back to ARGB.
              if (VERBOSE) {
                libyuv::I420ToARGB(reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()), width,
                                   reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(width * height)), width/2,
                                   reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(width * height + ((width * height) >> 2))), width/2,
                                   reinterpret_cast<uint8_t*>(rawABGRFrame.data()), width * 4,
                                   width, height);
              }

              // Check whether we need to initialize the window for viewing.
              if ((nullptr == display) && VERBOSE) {
                display = XOpenDisplay(NULL);
                visual = DefaultVisual(display, 0);
                window = XCreateSimpleWindow(display, RootWindow(display, 0), 0, 0, width, height, 1, 0, 0);
                ximage = XCreateImage(display, visual, 24, ZPixmap, 0, reinterpret_cast<char*>(rawABGRFrame.data()), width, height, 32, 0);
                XMapWindow(display, window);
              }

              // Show the image.
              if (VERBOSE) {
                XPutImage(display, window, DefaultGC(display, 0), ximage, 0, 0, 0, 0, width, height);
              }
            }
            sharedMemoryFori420->unlock();

            // Next, inform any downstream processes of the new frame that is ready.
            hasReceivedImageReading.store(false);
            before = cluon::time::now();
            sharedMemoryFori420->notifyAll();

            // Wait for the encoded response.
            {
                using namespace std::literals::chrono_literals;
                while (!hasReceivedImageReading.load() && !cluon::TerminateHandler::instance().isTerminated.load()) {
                  std::this_thread::sleep_for(1ms);
                }
                after = cluon::time::now();
            }
            if (VERBOSE) {
              std::clog << "[frame-feed-evaluator]: Received " << imageReading.fourcc() << " of size " << imageReading.data().size() << std::endl;
            }

            if ("h264" == imageReading.fourcc()) {
              // Unpack "h264" frame and calculate PSNR.
              std::string h264Frame{imageReading.data()};
              const uint32_t LEN{static_cast<uint32_t>(h264Frame.size())};
              if (0 < LEN) {
                uint8_t* yuvData[3];

                SBufferInfo bufferInfo;
                memset(&bufferInfo, 0, sizeof (SBufferInfo));

                if (0 != openh264Decoder->DecodeFrame2(reinterpret_cast<const unsigned char*>(h264Frame.c_str()), LEN, yuvData, &bufferInfo)) {
                  std::cerr << "[frame-feed-evaluator]: h264 decoding for current frame failed." << std::endl;
                }
                else {
                  if (1 == bufferInfo.iBufferStatus) {
                    double PSNR = 
libyuv::I420Psnr(reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()), width,
               reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(width * height)), width/2,
               reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(width * height + ((width * height) >> 2))), width/2,
               yuvData[0], bufferInfo.UsrData.sSystemBuffer.iStride[0],
               yuvData[1], bufferInfo.UsrData.sSystemBuffer.iStride[1],
               yuvData[2], bufferInfo.UsrData.sSystemBuffer.iStride[1],
               width, height);

                    double SSIM = 
libyuv::I420Ssim(reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()), width,
               reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(width * height)), width/2,
               reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(width * height + ((width * height) >> 2))), width/2,
               yuvData[0], bufferInfo.UsrData.sSystemBuffer.iStride[0],
               yuvData[1], bufferInfo.UsrData.sSystemBuffer.iStride[1],
               yuvData[2], bufferInfo.UsrData.sSystemBuffer.iStride[1],
               width, height);

                    std::clog << "[frame-feed-evaluator]: " << filename << ";" << width << ";" << height << ";" << LEN << ";" << "PSNR=" << PSNR << ";SSIM=" << SSIM << ";duration(microseconds)=" << cluon::time::deltaInMicroseconds(after, before) << std::endl;
                  }
                }
              }
            }
          }
          else {
            std::cerr << "[frame-feed-evaluator]: Error while loading '" << filename << "': " << lodepng_error_text(lodePNGRetVal) << std::endl;
          }

          // Delay playback if desired.
          if (0 < DELAY) {
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(DELAY));
          }
        }
      }
    }

    if (openh264Decoder) {
        openh264Decoder->Uninitialize();
        WelsDestroyDecoder(openh264Decoder);
    }

    if (nullptr != display) {
      XCloseDisplay(display);
    }
    retCode = 0;
  }
  return retCode;
}

