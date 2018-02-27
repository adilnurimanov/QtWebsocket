QT += network

INCLUDEPATH += $$PWD/

SOURCES += \
    $$PWD/QWsServer.cpp \
    $$PWD/QWsSocket.cpp \
    $$PWD/QWsHandshake.cpp \
    $$PWD/QWsFrame.cpp \
    $$PWD/QTlsServer.cpp \
    $$PWD/functions.cpp

HEADERS += \
    $$PWD/QWsServer.h \
    $$PWD/QWsSocket.h \
    $$PWD/QWsHandshake.h \
    $$PWD/QWsFrame.h \
    $$PWD/QTlsServer.h \
    $$PWD/functions.h \
    $$PWD/WsEnums.h

