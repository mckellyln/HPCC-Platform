################################################################################
#    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
################################################################################

# Component: testsocket
#####################################################
# Description:
# ------------
#    Cmake Input File for testsocket
#####################################################

project( testsocket ) 

include(${HPCC_SOURCE_DIR}/esp/scm/smcscm.cmake)

ADD_DEFINITIONS( -D_CONSOLE )

set ( SRCS
      ${ESPSCM_GENERATED_DIR}/ws_dfu_esp.cpp
      ${ESPSCM_GENERATED_DIR}/ws_fs_esp.cpp
      testsocket.cpp
    )

include_directories (            
         ./../../system/jlib
         ./../../system/include 
         ./../../system/security/securesocket
         ./../../system/security/shared
         ./../../esp/bindings/SOAP/xpp
         ./../dfu
         ./../ft
         ./../../system/xmllib
         ./../../esp/platform
         ./../../esp/clients
         ./../../rtl/include
         ./../../system/include
         ./../../system/jlib
         ./../../esp/bindings
         ./../../common/workunit
         ./../../common/remote
         ${CMAKE_BINARY_DIR}
         ${CMAKE_BINARY_DIR}/oss
    )

HPCC_ADD_EXECUTABLE ( testsocket ${SRCS} )

add_dependencies ( testsocket espscm )

target_link_libraries ( testsocket
         jlib
         remote
         xmllib
         esphttp
         )

IF (USE_OPENSSL)
    target_link_libraries ( testsocket
         securesocket
         )
ENDIF()

install ( TARGETS testsocket RUNTIME DESTINATION ${EXEC_DIR} )

if ( PLATFORM )
    install ( PROGRAMS testsocket.install DESTINATION etc/init.d/install COMPONENT Runtime )
    install ( PROGRAMS testsocket.uninstall DESTINATION etc/init.d/uninstall COMPONENT Runtime )
endif()
