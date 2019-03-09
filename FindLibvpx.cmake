# Copyright (C) 2018  Christian Berger
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

###########################################################################
# Find libvpx.
FIND_PATH(VPX_INCLUDE_DIR
          NAMES vpx/vpx_encoder.h
          PATHS /usr/local/include/
                /usr/include/)
MARK_AS_ADVANCED(VPX_INCLUDE_DIR)
FIND_LIBRARY(VPX_LIBRARY
             NAMES vpx
             PATHS ${LIBVPXDIR}/lib/
                    /usr/lib/arm-linux-gnueabihf/
                    /usr/lib/arm-linux-gnueabi/
                    /usr/lib/x86_64-linux-gnu/
                    /usr/local/lib64/
                    /usr/lib64/
                    /usr/lib/)
MARK_AS_ADVANCED(VPX_LIBRARY)

###########################################################################
IF (VPX_INCLUDE_DIR
    AND VPX_LIBRARY)
    SET(VPX_FOUND 1)
    SET(VPX_LIBRARIES ${VPX_LIBRARY})
    SET(VPX_INCLUDE_DIRS ${VPX_INCLUDE_DIR})
ENDIF()

MARK_AS_ADVANCED(VPX_LIBRARIES)
MARK_AS_ADVANCED(VPX_INCLUDE_DIRS)

IF (VPX_FOUND)
    MESSAGE(STATUS "Found libvpx: ${VPX_INCLUDE_DIRS}, ${VPX_LIBRARIES}")
ELSE ()
    MESSAGE(STATUS "Could not find libvpx")
ENDIF()
