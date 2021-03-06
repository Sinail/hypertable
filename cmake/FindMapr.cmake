# Copyright (C) 2007-2012 Hypertable, Inc.
#
# This file is part of Hypertable.
#
# Hypertable is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 3
# of the License, or any later version.
#
# Hypertable is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Hypertable. If not, see <http://www.gnu.org/licenses/>
#

# - Find Mapr
# Find the native Mapr includes and library
#
#  Mapr_INCLUDE_DIR - where to find Mapr.h, etc.
#  Mapr_LIBRARIES   - List of libraries when using Mapr.
#  Mapr_FOUND       - True if Mapr found.


if (Mapr_INCLUDE_DIR)
  # Already in cache, be silent
  set(Mapr_FIND_QUIETLY TRUE)
endif ()

find_path(Mapr_INCLUDE_DIR hdfs.h
  /opt/mapr/hadoop/hadoop-0.20.2/src/c++/libhdfs
)

macro(FIND_MAPR_LIB lib)
  find_library(${lib}_LIB NAMES ${lib}
    PATHS /opt/mapr/lib $ENV{JAVA_HOME}/jre/lib/amd64/server  )
  mark_as_advanced(${lib}_LIB)
endmacro(FIND_MAPR_LIB lib libname)

FIND_MAPR_LIB(MapRClient)
FIND_MAPR_LIB(jvm)

if (Mapr_INCLUDE_DIR AND MapRClient_LIB AND jvm_LIB)
  set(Mapr_FOUND TRUE)
  set(Mapr_LIBRARIES ${MapRClient_LIB} ${jvm_LIB})
else ()
   set(Mapr_FOUND FALSE)
   set(Mapr_LIBRARIES)
endif ()

if (Mapr_FOUND)
   message(STATUS "Found MAPR: ${Mapr_LIBRARIES}")
   if (NOT Mapr_FIND_QUIETLY)
      message(STATUS "Found MAPR: ${Mapr_LIBRARIES}")
   endif ()
else ()
   if (Mapr_FIND_REQUIRED)
      message(FATAL_ERROR "Could NOT find MAPR libraries")
   endif ()
endif ()

mark_as_advanced(
  Mapr_INCLUDE_DIR
)
