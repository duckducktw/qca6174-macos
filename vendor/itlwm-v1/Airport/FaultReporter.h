//
//  FaultReporter.h
//  Declarations for corecapture / IO80211Family fault reporting.
//  Mirrors the macOS 15 vendor copy, kept here so the Ventura IO80211 v1
//  build can register the same FaultReporter stream that
//  IO80211Controller::start() looks for through
//  findAndAttachToFaultReporter().
//

#ifndef WCLFaultReporter_h
#define WCLFaultReporter_h

#include "CCStream.h"
#include <IOKit/IOWorkLoop.h>

class CCDataStream : public CCStream {
    OSDeclareAbstractStructors(CCDataStream)
};

class CCFaultReporter : public IOService {
    OSDeclareDefaultStructors(CCFaultReporter)
public:
    static CCFaultReporter *withStreamWorkloop(CCDataStream *, IOWorkLoop *);
};

class CommonFaultReporter : public OSObject {
    OSDeclareAbstractStructors(CommonFaultReporter)
};

class IO80211FaultReporter : public CommonFaultReporter {
    OSDeclareDefaultStructors(IO80211FaultReporter)
public:
    static IO80211FaultReporter *allocWithParams(CCFaultReporter *);
};

#endif
