set(QT_DONT_USE_QTGUI TRUE)
set(QT_USE_QTNETWORK TRUE)
set(QT_USE_QTXML TRUE)
include(${QT_USE_FILE})

include_directories(../nat)

set(udpsender_SOURCES udpsend.cpp main.cpp)
set(udpsender_HEADERS udpsend.h)
qt4_wrap_cpp(udpsender_HEADERS_MOC ${udpsender_HEADERS})

add_executable(udpsender ${udpsender_SOURCES} ${udpsender_HEADERS_MOC})
target_link_libraries(udpsender upnp ${QT_LIBRARIES})
install(TARGETS udpsender
	RUNTIME DESTINATION tools)