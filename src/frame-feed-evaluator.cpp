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

#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
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
    auto cropCounter{
        commandlineArguments.count("crop.x") +
        commandlineArguments.count("crop.y") +
        commandlineArguments.count("crop.width") +
        commandlineArguments.count("crop.height")
    };
  if ( (0 == commandlineArguments.count("folder")) ||
       (0 == commandlineArguments.count("name")) ||
       ( (0 != cropCounter) && (4 != cropCounter) ) ||
       (0 == commandlineArguments.count("cid")) ) {
    std::cerr << argv[0] << " 'replays' a sequence of *.png files into i420 frames and waits for an ImageReading response before next frame." << std::endl;
    std::cerr << "Usage:   " << argv[0] << " --folder=<Folder with *.png files to replay> [--verbose]" << std::endl;
    std::cerr << "         --folder:          path to a folder with .png files" << std::endl;
    std::cerr << "         --crop.x:          crop this area from the input image (x for top left)" << std::endl;
    std::cerr << "         --crop.y:          crop this area from the input image (y for top left)" << std::endl;
    std::cerr << "         --crop.width:      crop this area from the input image (width)" << std::endl;
    std::cerr << "         --crop.height:     crop this area from the input image (height)" << std::endl;
    std::cerr << "         --name:            name of the shared memory area to create for i420 frame" << std::endl;
    std::cerr << "         --cid:             CID of the OD4Session to listen for encoded h264 frames" << std::endl;
    std::cerr << "         --delay:           delay between frames in ms; default: 1000" << std::endl;
    std::cerr << "         --delay.start:     delay before the first frame is replayed in ms; default: 5000" << std::endl;
    std::cerr << "         --timeout:         timeout in ms for waiting for encoded frame; default: 40ms (25fps)" << std::endl;
    std::cerr << "         --noexitontimeout: do not end program on timeout" << std::endl;
    std::cerr << "         --stopafter:       process only the first n frames (n > 0); default: 0 (process all)" << std::endl;
    std::cerr << "         --savepng:         flag to store decoded lossy frames as .png; default: false" << std::endl;
    std::cerr << "         --report:          name of the file for the report" << std::endl;
    std::cerr << "         --verbose:         sourceFrameDisplay PNG frame while replaying" << std::endl;
    std::cerr << "Example: " << argv[0] << " --folder=. --verbose" << std::endl;
    retCode = 1;
  } else {
    const std::string folderWithPNGs{commandlineArguments["folder"]};
    const uint32_t CROP_X{(commandlineArguments.count("crop.x") != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["crop.x"])) : 0};
    const uint32_t CROP_Y{(commandlineArguments.count("crop.y") != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["crop.y"])) : 0};
    const uint32_t CROP_WIDTH{(commandlineArguments.count("crop.width") != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["crop.width"])) : 0};
    const uint32_t CROP_HEIGHT{(commandlineArguments.count("crop.height") != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["crop.height"])) : 0};
    const std::string REPORT{commandlineArguments["report"]};
    const std::string NAME{commandlineArguments["name"]};
    const uint32_t DELAY_START{(commandlineArguments["delay.start"].size() != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["delay.start"])) : 5000};
    const uint32_t DELAY{(commandlineArguments["delay"].size() != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["delay"])) : 1000};
    const uint32_t TIMEOUT{(commandlineArguments["timeout"].size() != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["timeout"])) : 40};
    const bool VERBOSE{commandlineArguments.count("verbose") != 0};
    const bool EXIT_ON_TIMEOUT{commandlineArguments.count("noexitontimeout") == 0};
    const uint32_t STOPAFTER{(commandlineArguments["stopafter"].size() != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["stopafter"])) : 0};
    const bool SAVE_PNG{commandlineArguments.count("savepng") == 0};

    // Show frames.
    Display *sourceFrameDisplay{nullptr};
    Visual *sourceFrameVisual{nullptr};
    Window sourceFrameWindow{0};
    XImage *sourceFrameXImage{nullptr};

    Display *resultingFrameDisplay{nullptr};
    Visual *resultingFrameVisual{nullptr};
    Window resultingFrameWindow{0};
    XImage *resultingFrameXImage{nullptr};

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

    bool vpxCodecInitialized{false};
    vpx_codec_ctx_t codec;

    // Frame data.
    std::vector<unsigned char> rawABGRFromPNG;
    std::vector<unsigned char> rawARGBFrame;
    std::vector<unsigned char> tempImageBuffer;
    std::unique_ptr<cluon::SharedMemory> sharedMemoryFori420{nullptr};
    std::vector<unsigned char> resultingI420Frame;
    std::vector<unsigned char> resultingRawARGBFrame;

    cluon::OD4Session od4{static_cast<uint16_t>(std::stoi(commandlineArguments["cid"]))};
    if (od4.isRunning()) {
      cluon::data::TimeStamp before, after;
      std::atomic<bool> hasReceivedImageReading{false};
      opendlv::proxy::ImageReading imageReading;
      od4.dataTrigger(opendlv::proxy::ImageReading::ID(), [&hasReceivedImageReading, &imageReading, &after](cluon::data::Envelope &&env){
        if (opendlv::proxy::ImageReading::ID() == env.dataType()) {
          after = env.sent();
          imageReading = cluon::extractMessage<opendlv::proxy::ImageReading>(std::move(env));
          hasReceivedImageReading.store(true);
        }
      });

      std::unique_ptr<std::fstream> reportFile{nullptr};
      if (!REPORT.empty()) {
        reportFile.reset(new std::fstream(REPORT.c_str(), std::ios::trunc|std::ios::out));
        if (!(reportFile && reportFile->good())) {
          reportFile = nullptr;
        }
      }

      // Sort file entries.
      std::vector<std::string> entries;
      for (const auto &entry : std::filesystem::directory_iterator(folderWithPNGs)) {
        std::string filename{entry.path()};
        if (std::string::npos != filename.find(".png")) {
          entries.push_back(filename);
        }
      }
      std::sort(entries.begin(), entries.end());

      uint32_t width{0}, height{0};
      uint32_t finalWidth{CROP_WIDTH}, finalHeight{CROP_HEIGHT};
      uint32_t entryCounter{0};
      for (const auto &entry : entries) {
        entryCounter++;
        std::string filename{entry};
        if (VERBOSE) {
          std::clog << "[frame-feed-evaluator]: Processing " << entryCounter << "/" << entries.size() << ": '"  << filename << "'." << std::endl;
        }

        // Reset raw buffer for PNG.
        rawABGRFromPNG.clear();
        unsigned lodePNGRetVal = lodepng::decode(rawABGRFromPNG, width, height, filename.c_str());
        if (0 == lodePNGRetVal) {
          rawARGBFrame.reserve(rawABGRFromPNG.size());
          resultingRawARGBFrame.reserve(rawABGRFromPNG.size());

          // Initialize output frame in i420 format.
          if (!sharedMemoryFori420) {
            if (0 == (finalWidth * finalHeight)) {
              finalWidth = width;
              finalHeight = height;
            }

            tempImageBuffer.reserve(width * height * 3/2);
            sharedMemoryFori420.reset(new cluon::SharedMemory{NAME, finalWidth * finalHeight * 3/2});
            std::clog << "[frame-feed-evaluator]: Created shared memory '" << NAME << "' of size " << sharedMemoryFori420->size() << " holding an i420 frame of size " << finalWidth << "x" << finalHeight << "." << std::endl;
            resultingI420Frame.reserve(sharedMemoryFori420->size());

            // Once the shared memory is created, wait for the first frame to replay
            // so that any downstream processes can attach to it.
            if (0 < DELAY_START) {
              std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(DELAY_START));
            }
          }

          // Exclusive access to shared memory.
          sharedMemoryFori420->lock();
          {
            // First, transform original image into tempory buffer.
            libyuv::ABGRToI420(reinterpret_cast<uint8_t*>(rawABGRFromPNG.data()), width * 4 /* 4*WIDTH for ABGR*/,
                               reinterpret_cast<uint8_t*>(tempImageBuffer.data()), width,
                               reinterpret_cast<uint8_t*>(tempImageBuffer.data()+(width * height)), width/2,
                               reinterpret_cast<uint8_t*>(tempImageBuffer.data()+(width * height + ((width * height) >> 2))), width/2,
                               width, height);

            // Next, crop input image to desired dimensions.
            libyuv::ConvertToI420(reinterpret_cast<uint8_t*>(tempImageBuffer.data()), width * height * 3/2 /* 3/2*width for I420*/,
                                  reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()), finalWidth,
                                  reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(finalWidth * finalHeight)), finalWidth/2,
                                  reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(finalWidth * finalHeight + ((finalWidth * finalHeight) >> 2))), finalWidth/2,
                                  CROP_X, CROP_Y,
                                  width, height,
                                  finalWidth, finalHeight,
                                  static_cast<libyuv::RotationMode>(0), FOURCC('I', '4', '2', '0'));

            // When we need to show the image, transform from i420 back to ARGB.
            if (VERBOSE) {
              libyuv::I420ToARGB(reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()), finalWidth,
                                 reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(finalWidth * finalHeight)), finalWidth/2,
                                 reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(finalWidth * finalHeight + ((finalWidth * finalHeight) >> 2))), finalWidth/2,
                                 reinterpret_cast<uint8_t*>(rawARGBFrame.data()), finalWidth * 4,
                                 finalWidth, finalHeight);
            }

            // Check whether we need to initialize the sourceFrameWindow for viewing.
            if ((nullptr == sourceFrameDisplay) && VERBOSE) {
              sourceFrameDisplay = XOpenDisplay(NULL);
              sourceFrameVisual = DefaultVisual(sourceFrameDisplay, 0);
              sourceFrameWindow = XCreateSimpleWindow(sourceFrameDisplay, RootWindow(sourceFrameDisplay, 0), 0, 0, finalWidth, finalHeight, 1, 0, 0);
              sourceFrameXImage = XCreateImage(sourceFrameDisplay, sourceFrameVisual, 24, ZPixmap, 0, reinterpret_cast<char*>(rawARGBFrame.data()), finalWidth, finalHeight, 32, 0);
              XMapWindow(sourceFrameDisplay, sourceFrameWindow);
            }

            // Show the image.
            if (VERBOSE) {
              XPutImage(sourceFrameDisplay, sourceFrameWindow, DefaultGC(sourceFrameDisplay, 0), sourceFrameXImage, 0, 0, 0, 0, finalWidth, finalHeight);
            }

            // Check whether we need to initialize the resultingFrameWindow for viewing.
            if ((nullptr == resultingFrameDisplay) && VERBOSE) {
              resultingFrameDisplay = XOpenDisplay(NULL);
              resultingFrameVisual = DefaultVisual(resultingFrameDisplay, 0);
              resultingFrameWindow = XCreateSimpleWindow(resultingFrameDisplay, RootWindow(resultingFrameDisplay, 0), 0, 0, finalWidth, finalHeight, 1, 0, 0);
              resultingFrameXImage = XCreateImage(resultingFrameDisplay, resultingFrameVisual, 24, ZPixmap, 0, reinterpret_cast<char*>(resultingRawARGBFrame.data()), finalWidth, finalHeight, 32, 0);
              XMapWindow(resultingFrameDisplay, resultingFrameWindow);
            }
          }
          sharedMemoryFori420->unlock();

          // Next, inform any downstream processes of the new frame that is ready.
          hasReceivedImageReading.store(false);
          before = cluon::time::now();
          sharedMemoryFori420->setTimeStamp(before);
          sharedMemoryFori420->notifyAll();

          // Wait for the encoded response.
          {
              uint32_t timeout{TIMEOUT};
              using namespace std::literals::chrono_literals;
              while (!hasReceivedImageReading.load() &&
                     !cluon::TerminateHandler::instance().isTerminated.load() &&
                     (0 < timeout)) {
                std::this_thread::sleep_for(1ms);
                timeout--;
              }
              if ((0 == timeout) && !hasReceivedImageReading.load()) {
                std::cerr << "[frame-feed-evaluator]: Timed out while waiting for encoded frame." << std::endl;
                if (EXIT_ON_TIMEOUT) {
                  return retCode;
                }
              }
          }
          if (VERBOSE) {
            std::clog << "[frame-feed-evaluator]: Received " << imageReading.fourcc() << " of size " << imageReading.data().size() << std::endl;
          }

          bool frameDecodedSuccessfully{false};
          std::string compressedFrame{imageReading.data()};
          const uint32_t LEN{static_cast<uint32_t>(compressedFrame.size())};

          if ( ("VP80" == imageReading.fourcc()) || ("VP90" == imageReading.fourcc()) ) {
            // Unpack VPx frame.
            if (!vpxCodecInitialized) {
              if ("VP80" == imageReading.fourcc()) {
                if (!vpx_codec_dec_init(&codec, &vpx_codec_vp8_dx_algo, nullptr, 0)) {
                  std::clog << "[frame-feed-evaluator]: Using " << vpx_codec_iface_name(&vpx_codec_vp8_dx_algo) << std::endl;
                  vpxCodecInitialized = true;
                }
              }
              if ("VP90" == imageReading.fourcc()) {
                if (!vpx_codec_dec_init(&codec, &vpx_codec_vp9_dx_algo, nullptr, 0)) {
                  std::clog << "[frame-feed-evaluator]: Using " << vpx_codec_iface_name(&vpx_codec_vp9_dx_algo) << std::endl;
                  vpxCodecInitialized = true;
                }
              }
            }
            if (vpxCodecInitialized) {
              if (0 < LEN) {
                if (vpx_codec_decode(&codec, reinterpret_cast<const unsigned char*>(compressedFrame.c_str()), LEN, nullptr, 0)) {
                  std::cerr << "[frame-feed-evaluator]: Decoding for current frame failed." << std::endl;
                }
                else {
                  frameDecodedSuccessfully = true;

                  vpx_codec_iter_t it{nullptr};
                  vpx_image_t *yuvFrame{nullptr};
                  while (nullptr != (yuvFrame = vpx_codec_get_frame(&codec, &it))) {
                    libyuv::I420Copy(yuvFrame->planes[VPX_PLANE_Y], yuvFrame->stride[VPX_PLANE_Y],
                                     yuvFrame->planes[VPX_PLANE_U], yuvFrame->stride[VPX_PLANE_U],
                                     yuvFrame->planes[VPX_PLANE_V], yuvFrame->stride[VPX_PLANE_V],
                                     reinterpret_cast<uint8_t*>(resultingI420Frame.data()), finalWidth,
                                     reinterpret_cast<uint8_t*>(resultingI420Frame.data()+(finalWidth * finalHeight)), finalWidth/2,
                                     reinterpret_cast<uint8_t*>(resultingI420Frame.data()+(finalWidth * finalHeight + ((finalWidth * finalHeight) >> 2))), finalWidth/2,
                                     finalWidth, finalHeight);

                    if (VERBOSE) {
                      libyuv::I420ToARGB(yuvFrame->planes[VPX_PLANE_Y], yuvFrame->stride[VPX_PLANE_Y],
                                         yuvFrame->planes[VPX_PLANE_U], yuvFrame->stride[VPX_PLANE_U],
                                         yuvFrame->planes[VPX_PLANE_V], yuvFrame->stride[VPX_PLANE_V],
                                         reinterpret_cast<uint8_t*>(resultingRawARGBFrame.data()), finalWidth * 4,
                                         finalWidth, finalHeight);
                      XPutImage(resultingFrameDisplay, resultingFrameWindow, DefaultGC(resultingFrameDisplay, 0), resultingFrameXImage, 0, 0, 0, 0, finalWidth, finalHeight);
                    }

                  }
                }
              }
            }
          }
          else if ("h264" == imageReading.fourcc()) {
            // Unpack "h264" frame.
            if (0 < LEN) {
              uint8_t* yuvData[3];
              SBufferInfo bufferInfo;
              memset(&bufferInfo, 0, sizeof (SBufferInfo));
              if (0 != openh264Decoder->DecodeFrame2(reinterpret_cast<const unsigned char*>(compressedFrame.c_str()), LEN, yuvData, &bufferInfo)) {
                std::cerr << "[frame-feed-evaluator]: h264 decoding for current frame failed." << std::endl;
              }
              else {
                if (1 == bufferInfo.iBufferStatus) {
                  libyuv::I420Copy(yuvData[0], bufferInfo.UsrData.sSystemBuffer.iStride[0],
                                   yuvData[1], bufferInfo.UsrData.sSystemBuffer.iStride[1],
                                   yuvData[2], bufferInfo.UsrData.sSystemBuffer.iStride[1],
                                   reinterpret_cast<uint8_t*>(resultingI420Frame.data()), finalWidth,
                                   reinterpret_cast<uint8_t*>(resultingI420Frame.data()+(finalWidth * finalHeight)), finalWidth/2,
                                   reinterpret_cast<uint8_t*>(resultingI420Frame.data()+(finalWidth * finalHeight + ((finalWidth * finalHeight) >> 2))), finalWidth/2,
                                   finalWidth, finalHeight);

                  if (VERBOSE) {
                    libyuv::I420ToARGB(yuvData[0], bufferInfo.UsrData.sSystemBuffer.iStride[0],
                                       yuvData[1], bufferInfo.UsrData.sSystemBuffer.iStride[1],
                                       yuvData[2], bufferInfo.UsrData.sSystemBuffer.iStride[1],
                                       reinterpret_cast<uint8_t*>(resultingRawARGBFrame.data()), finalWidth * 4,
                                       finalWidth, finalHeight);
                    XPutImage(resultingFrameDisplay, resultingFrameWindow, DefaultGC(resultingFrameDisplay, 0), resultingFrameXImage, 0, 0, 0, 0, finalWidth, finalHeight);
                  }
                  frameDecodedSuccessfully = true;
                }
              }
            }
          }

          // Compute PSNR/SSIM.
          if (frameDecodedSuccessfully) {
            // Show the results.
            double PSNR =
libyuv::I420Psnr(reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()), finalWidth,
             reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(finalWidth * finalHeight)), finalWidth/2,
             reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(finalWidth * finalHeight + ((finalWidth * finalHeight) >> 2))), finalWidth/2,
             reinterpret_cast<uint8_t*>(resultingI420Frame.data()), finalWidth,
             reinterpret_cast<uint8_t*>(resultingI420Frame.data()+(finalWidth*finalHeight)), finalWidth/2,
             reinterpret_cast<uint8_t*>(resultingI420Frame.data()+(finalWidth*finalHeight)+(finalWidth*finalHeight)/4), finalWidth/2,
             finalWidth, finalHeight);

            double SSIM =
libyuv::I420Ssim(reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()), finalWidth,
             reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(finalWidth * finalHeight)), finalWidth/2,
             reinterpret_cast<uint8_t*>(sharedMemoryFori420->data()+(finalWidth * finalHeight + ((finalWidth * finalHeight) >> 2))), finalWidth/2,
             reinterpret_cast<uint8_t*>(resultingI420Frame.data()), finalWidth,
             reinterpret_cast<uint8_t*>(resultingI420Frame.data()+(finalWidth*finalHeight)), finalWidth/2,
             reinterpret_cast<uint8_t*>(resultingI420Frame.data()+(finalWidth*finalHeight)+(finalWidth*finalHeight)/4), finalWidth/2,
             finalWidth, finalHeight);

            if (SAVE_PNG) {
              std::vector<unsigned char> image;
              image.resize(finalWidth * finalHeight * 4);

              if (-1 == libyuv::I420ToABGR(resultingI420Frame.data(), finalWidth,
                                           resultingI420Frame.data()+(finalWidth * finalHeight), finalWidth/2,
                                           resultingI420Frame.data()+(finalWidth * finalHeight + ((finalWidth * finalHeight) >> 2)), finalWidth/2,
                                           image.data(), finalWidth * 4,
                                           finalWidth, finalHeight) ) {
                  std::cerr << "[frame-feed-evaluator]: Error transforming color space." << std::endl;
              }
              else {
                  std::stringstream tmp;
                  tmp << "lossy_" << std::setw(10) << std::setfill('0') << entryCounter << std::setfill(' ') << ".png";
                  const std::string str = tmp.str();
                  auto r = lodepng::encode(str.c_str(), image, finalWidth, finalHeight);
                  if (r) {
                      std::cerr << "[frame-feed-evaluator]: lodePNG error " << r << ": "<< lodepng_error_text(r) << std::endl;
                  }
              }
            }

            std::stringstream sstr;
            sstr << "[frame-feed-evaluator]: " << filename << ";" << CROP_X << ";" << CROP_Y << ";" << finalWidth << ";" << finalHeight << ";size[bytes];" << LEN << ";" << "PSNR;" << PSNR << ";SSIM;" << SSIM << ";duration[microseconds];" << cluon::time::deltaInMicroseconds(after, before);
            const std::string str = sstr.str();
            if (VERBOSE) {
              std::clog << str << std::endl;
            }
            if (reportFile && reportFile->good()) {
              *reportFile << str << std::endl;
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

        // End processing if desired.
        if ( (STOPAFTER > 0) && (entryCounter > STOPAFTER)) {
          break;
        }
      }
    }

    if (openh264Decoder) {
        openh264Decoder->Uninitialize();
        WelsDestroyDecoder(openh264Decoder);
    }

    if (nullptr != sourceFrameDisplay) {
      XCloseDisplay(sourceFrameDisplay);
    }
    if (nullptr != resultingFrameDisplay) {
      XCloseDisplay(resultingFrameDisplay);
    }
    retCode = 0;
  }
  return retCode;
}

