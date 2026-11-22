// Ventura-era (IO80211 v1) private ABI selection for the ath10k driver.
//
// The Sequoia build compiles against the IO80211ControllerV2 / Skywalk surface.
// Ventura (macOS 13) predates that and drives Wi-Fi through the classic
// IO80211Controller + IO80211Interface + apple80211 request pair, so the same
// radio core is compiled against those declarations instead.
#pragma once

#ifndef ATH10K_VENTURA
#error "Ath10kVenturaABI.hpp is only valid in the ATH10K_VENTURA build"
#endif

// Selects the header branches for macOS 13.x in the vendored declarations.
#ifndef __IO80211_TARGET
#define __IO80211_TARGET __MAC_13_0
#endif

#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>

#include "IO80211Controller.h"
#include "IO80211WorkQueue.h"
#include "IO80211Interface.h"
#include "apple80211_var.h"
#include "apple80211_ioctl.h"
#include "CCPipe.h"
#include "CCStream.h"
#include "CCLogStream.h"
#include "FaultReporter.h"
