#!/usr/bin/env python3
"""Focused portable regressions; not a substitute for a macOS kext/hardware test."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', type=Path, help='Zig executable for Windows host tests')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--only', nargs='+', help='Run selected portable C++ tests; this is not a full-suite result')
    args = parser.parse_args()
    out = ROOT / 'build/development-tests'
    out.mkdir(parents=True, exist_ok=True)
    passed = []
    # UBSan otherwise recovers and can leave a successful process exit code.
    # A sanitizer finding must fail the regression, including in CI.
    test_env = dict(os.environ)
    test_env['UBSAN_OPTIONS'] = test_env.get('UBSAN_OPTIONS', '') + ':halt_on_error=1'
    def run(name, command):
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, errors='replace', env=test_env)
        (out / (name + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        if result.returncode:
            raise RuntimeError(f'{name} failed; see {out / (name + ".log")}')
        print('PASS', name, flush=True)
        passed.append(name)
    run('prepare-firmware', [sys.executable, 'scripts/prepare-firmware.py'])
    run('payload-python', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_payload.py', '-v'])
    run('kext-symbols', [sys.executable, 'tests/test_kext_symbols.py', '-v'])
    run('kernel-collection-inclusion', [sys.executable, 'tests/test_kernel_collection.py', '-v'])
    run('p256-diagnostic-config', [sys.executable, 'tests/test_p256_diagnostic_config.py', '-v'])
    run('native-lock-order', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_native_lock_order.py', '-v'])
    run('diagnostic-privacy', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_native_preflight.py', '-v'])
    if args.zig:
        compiler = [str(args.zig.resolve()), 'c++', '-target', 'x86_64-windows-gnu']
    else:
        compiler = shlex.split(args.cxx)
    flags = ['-std=c++17', '-O2', '-UNDEBUG', '-Wall', '-Wextra', '-Werror', '-Wno-nullability-completeness']
    with tempfile.TemporaryDirectory(prefix='ath10k-tests-') as tmp:
        include = Path(tmp)
        (include/'IOKit').mkdir()
        (include/'IOKit/IOLib.h').write_bytes((ROOT/'tests/mock_iolib.h').read_bytes())
        (include/'IOKit/IOTypes.h').write_text('#pragma once\n#include <cstdint>\nusing UInt8=uint8_t;using UInt16=uint16_t;using UInt32=uint32_t;using UInt64=uint64_t;\n')
        run('extract-production', [sys.executable, 'tests/extract-production.py', str(include/'driver_methods.inc')])
        power=(ROOT/'src/kext/Ath10kPower.cpp').read_text()
        (include/'power_methods.inc').write_text(power[power.index('bool Ath10kPCIDevice::preparePowerNotifications('):])
        native=(ROOT/'src/kext/native/Ath10kNative.cpp').read_text()
        driver=(ROOT/'src/kext/Ath10kPCIDevice.cpp').read_text()
        scan_service=driver[driver.index('const char *Ath10kPCIDevice::serviceWMIScan(UInt64 nowMs)'):]
        scan_service=scan_service[:scan_service.index('    if (!state.attempted && (_connection.stats.step')]
        (include/'native_scan_start.inc').write_text(scan_service+'    (void)nowMs;++submissions;return nullptr;\n}\n')
        (include/'native_termination_power.inc').write_text(power[power.index('void Ath10kPCIDevice::blockPowerIOForTermination()'):power.index('bool Ath10kPCIDevice::controllerPowerClosed()')])
        (include/'native_termination_controller.inc').write_text(native[native.index('void Ath10kPCIDevice::prepareNativeInterfaceTermination('):native.index('void Ath10kPCIDevice::stopNativeRequests()')])
        (include/'native_termination_notifier.inc').write_text(native[native.index('bool Ath10kNativeInterface::prepareTerminationNotification()'):native.index('bool Ath10kNativeInterface::prepareRegistrationInfo(')])
        (include/'native_bsd_reference.inc').write_text(native[native.index('IOReturn Ath10kNativeInterface::prepareBSDInterface('):native.index('bool Ath10kNativeInterface::prepareTerminationNotification()')])
        (include/'native_termination_free.inc').write_text(native[native.index('void Ath10kNativeInterface::free()'):native.index('IOReturn Ath10kNativeInterface::deregisterLogicalLink()')])

        (include/'native_release_method.inc').write_text(native[native.index('void Ath10kPCIDevice::releaseNativeInterface()'):native.index('IOReturn Ath10kPCIDevice::nativeRequest(')])
        (include/'native_registration_methods.inc').write_text(native[native.index('bool Ath10kNativeInterface::prepareRegistrationInfo('):native.index('IOWorkLoop *Ath10kNativeInterface::getWorkLoop() const')])
        (include/'native_security_class.inc').write_text(native[native.index('class Ath10kPCIDevice::NativeSecurity {'):native.index('IO80211WorkQueue *Ath10kPCIDevice::getWorkQueue()')])
        terminal=native[native.index('    if(sec && (sec->ready ||'):native.index('    // Crypto/radio progress must not wait')]
        (include/'native_security_terminal_cleanup.inc').write_text('void Ath10kPCIDevice::discardIfStopped(){auto sec=_nativeSecurity;auto &s=_connection;\n'+terminal+'}\n')
        network_source=(ROOT/'src/kext/Ath10kNetwork.cpp').read_text()
        sae_stage=network_source[network_source.index('    if(s.stage==LinkStage::Auth && s.securityAKM==8){'):]
        sae_stage=sae_stage[:sae_stage.index('#endif')]
        (include/'native_sae_stage.inc').write_text('bool Ath10kPCIDevice::advanceSAEAuth(UInt64 nowMs){using Ath10k::LinkStage;auto &s=_connection;bool advanced=false;auto stage=[&]()->const char *{\n'+sae_stage+'\nadvanced=true;return nullptr;};stage();return advanced;}\n')
        before_key_build=network_source[:network_source.index('            UInt32 bytes=Ath10k::buildSecurityWMI(')]
        firmware_guard=before_key_build[before_key_build.rindex('            if(s.securityPhase==3 && !nativePMFPortReady())'):].splitlines()[0]
        (include/'native_pmf_firmware_guard.inc').write_text('const char *Ath10kPCIDevice::beforeFirmwareAuthorization(unsigned phase){struct {unsigned securityPhase;} s{phase};\n'+firmware_guard+'\nreturn nullptr;}\n')
        (include/'native_pmf_receive.inc').write_text(native[native.index('bool Ath10kPCIDevice::nativePMFManagement('):native.index('// Invoked by the radio-gated data-key state machine')])
        (include/'native_pmf_key_hooks.inc').write_text(native[native.index('bool Ath10kPCIDevice::nativeStageManagementKey('):native.index('// Radio-gated terminal cleanup.')])
        (include/'native_security_discard.inc').write_text(native[native.index('void Ath10kPCIDevice::discardNativeSecurity()'):native.index('IOReturn Ath10kPCIDevice::nativeStopAction(')])
        leave=native[native.index('    case N::setWCL_JOIN_ABORT:case N::setWCL_LEAVE_NETWORK:'):native.index('    case N::setCLEAR_PMKSA_CACHE:')]
        (include/'native_leave.inc').write_text(leave)
        request=native[native.index('IOReturn Ath10kPCIDevice::nativeRequest('):native.index('IOReturn Ath10kPCIDevice::nativeCurrentAPInfo(')]
        prefix=native[native.index('IOReturn Ath10kPCIDevice::nativeRequestGated('):native.index('    auto &s=_connection;using N=AthNativeOp;',native.index('IOReturn Ath10kPCIDevice::nativeRequestGated('))]
        (include/'native_request_admission.inc').write_text(request+prefix+'    (void)op;(void)data;++accepted;return kIOReturnSuccess;\n}\n')

        entry=driver[driver.index('void Ath10kPCIDevice::runDeferredProbe()'):driver.index('    ATH10K_TRACE("Ath10kMac: deferred target probe begin') ]
        (include/'first_probe_run.inc').write_text(entry+'    ++starts;IOLockUnlock(_probePowerLock);\n}\n')
        (include/'first_probe_pci.inc').write_text(driver[driver.index('bool Ath10kPCIDevice::preparePoweredPCI()'):driver.index('void Ath10kPCIDevice::queueDeferredProbe()')])
        recovery=(ROOT/'src/kext/Ath10kRecovery.cpp').read_text()
        (include/'wake_recovery_methods.inc').write_text(recovery[recovery.index('void Ath10kPCIDevice::queueWakeRecovery()'):])
        (include/'restore_aspm_methods.inc').write_text(driver[driver.index('bool Ath10kPCIDevice::disableASPMForBringup()'):driver.index('void Ath10kPCIDevice::logMSIState(')])
        (include/'first_probe_scheduler.inc').write_text(driver[driver.index('void Ath10kPCIDevice::queueDeferredProbe()'):driver.index('void Ath10kPCIDevice::deferredProbeCall(')])
        (include/'first_probe_callback.inc').write_text(driver[driver.index('void Ath10kPCIDevice::deferredProbeCall('):driver.index('void Ath10kPCIDevice::runDeferredProbe()')])
        (include/'first_probe_cancel.inc').write_text(driver[driver.index('void Ath10kPCIDevice::cancelDeferredProbe()'):driver.index('bool Ath10kPCIDevice::stopDMAForRelease()')])
        (include/'pci_match_method.inc').write_text(driver[driver.index('static bool ath10kRegistryPCIID('):driver.index('bool Ath10kPCIDevice::start(')])
        (include/'control_drain_method.inc').write_text(driver[driver.index('void Ath10kPCIDevice::drainControlWorkers()'):driver.index('void Ath10kPCIDevice::stop(IOService *provider)')])
        protocol=native[native.index('IOWorkLoop *Ath10kNativeInterface::getWorkLoop() const'):native.index('IOReturn Ath10kNativeInterface::perform')]
        bsd=native.split('class Ath10kBSDInterface',1)[1].split('    static IOReturn prepare(',1)[0]
        bsd=bsd.split('    IOWorkLoop *getWorkLoop() const override {',1)[1].rsplit('    }',1)[0]
        (include/'native_provider_methods.inc').write_text(protocol+'\nIOWorkLoop *Ath10kBSDInterface::getWorkLoop() const {'+bsd+'}\n')
        (include/'native_mac_method.inc').write_text(native[native.index('const char *Ath10kPCIDevice::serviceNativeMAC('):native.index('const char *Ath10kPCIDevice::nativeSelectBSS(')])
        (include/'native_bridge_methods.inc').write_text(native[native.index('void Ath10kPCIDevice::requestNativeService()'):native.index('void Ath10kPCIDevice::serviceNative(UInt64 now)')])
        (include/'native_bridge_stop.inc').write_text(native[native.index('IOReturn Ath10kPCIDevice::nativeStopAction('):native.index('void Ath10kPCIDevice::releaseNativeInterface()')])
        (include/'native_phy_capabilities.inc').write_text(native[native.index('IOReturn Ath10kPCIDevice::nativePHYCapabilities('):native.index('IOReturn Ath10kPCIDevice::nativeRequestGated(')])
        (include/'native_link_metrics.inc').write_text(native[native.index('IOReturn Ath10kPCIDevice::nativeLinkMetrics('):native.index('static_assert(sizeof(apple80211_key)')])
        (include/'native_link_state.inc').write_text(native[native.index('IOReturn Ath10kPCIDevice::nativeCurrentAPInfo('):native.index('static UInt32 channelFlags(')])
        (include/'native_notifications.inc').write_text(native[native.index('void Ath10kPCIDevice::serviceNativeNotifications('):native.index('// Runs under the network workloop gate;',native.index('void Ath10kPCIDevice::serviceNativeNotifications('))])
        worker_header=(ROOT/'src/kext/native/Ath10kSAEWorker.hpp').read_text()
        worker_source=(ROOT/'src/kext/native/Ath10kSAEWorker.cpp').read_text()
        worker='\n'.join(line for line in (worker_header+'\n'+worker_source).splitlines()
                         if not line.startswith('#include') and line!='#pragma once')
        (include/'sae_worker.inc').write_text(worker+'\n')
        session_sources=[ROOT/'src/kext/native/Ath10kSAESession.hpp', ROOT/'src/kext/native/Ath10kSAESession.cpp']
        session='\n'.join(line for source in session_sources for line in source.read_text().splitlines()
                          if not line.startswith('#include') and line!='#pragma once')
        (include/'sae_session.inc').write_text(session+'\n')
        preparation_sources=[ROOT/'src/kext/native/Ath10kNativeSAEPreparation.hpp',ROOT/'src/kext/native/Ath10kNativeSAEPreparation.cpp']
        preparation='\n'.join(line for source in preparation_sources for line in source.read_text().splitlines()
                               if not line.startswith('#include') and line!='#pragma once')
        (include/'native_sae_preparation.inc').write_text(preparation+'\n')
        p256_sources=[ROOT/'src/kext/native/Ath10kP256CheckJob.hpp',ROOT/'src/kext/native/Ath10kP256CheckJob.cpp']
        p256='\n'.join(line for source in p256_sources for line in source.read_text().splitlines()
                       if not line.startswith('#include') and line!='#pragma once')
        (include/'p256_check.inc').write_text(p256+'\n')
        p256_registry_sources=[ROOT/'src/kext/native/Ath10kP256RegistryJob.hpp',ROOT/'src/kext/native/Ath10kP256RegistryJob.cpp']
        p256_registry='\n'.join(line for source in p256_registry_sources for line in source.read_text().splitlines()
                                if not line.startswith('#include') and line!='#pragma once')
        (include/'p256_registry.inc').write_text(p256_registry+'\n')
        pmf_sources=[ROOT/'src/kext/native/Ath10kPMFSession.hpp',ROOT/'src/kext/native/Ath10kPMFSession.cpp']
        pmf='\n'.join(line for source in pmf_sources for line in source.read_text().splitlines()
                      if not line.startswith('#include') and line!='#pragma once')
        (include/'pmf_session.inc').write_text(pmf+'\n')
        names=['regulatory','dma_radio','wmi_radio','native_leave', 'connection', 'tx_watchdog', 'security_disconnect', 'rx_security_diagnostics', 'reconnect_090', 'station_maintenance', 'channel_switch', 'ap_maintenance', 'runtime_power', 'wmi_notifications', 'restore_aspm', 'wake_recovery', 'first_probe', 'pci_match', 'native_bsd_reference', 'native_termination', 'native_release', 'native_registration', 'native_request_admission', 'native_security_lifecycle', 'power', 'control_drain', 'native_provider', 'native_bridge', 'native_notifications', 'native_link_state', 'native_link_metrics', 'native_phy_capabilities', 'native_join', 'native_mac', 'native_scan', 'rsn_policy', 'pmf_transport', 'pmf_inbox', 'sa_query', 'sae_wire', 'sae_exchange', 'sae_worker', 'lunnen_payload', 'liteon_qca6174_payload', 'enterprise_policy', 'security', 'native_wire', 'native_status', 'native_capture']
        if sys.platform == 'darwin': names.extend(['p256_backend','sha256_supplicant','sae_session','sae_eapol','pmf_ccmp','pmf_session','native_pmf_receive','native_key_stage','native_sae_radio'])
        if args.only:
            unknown = sorted(set(args.only) - set(names))
            if unknown: parser.error('Unknown/unavailable tests: ' + ', '.join(unknown))
            names = [name for name in names if name in args.only]
        for name in names:
            executable = out / ('test_' + name + ('.exe' if os.name == 'nt' else ''))
            libraries = ['-lbcrypt'] if name == 'security' and os.name == 'nt' else (
                ['-lcrypto'] if name == 'security' and sys.platform != 'darwin' else [])
            run(name + '-compile', compiler + flags + (['-pthread'] if name in ['p256_backend','sae_session','pmf_session','native_pmf_receive','sae_worker','power','native_termination','native_bridge','first_probe','wake_recovery','connection','tx_watchdog','security_disconnect','reconnect_090','station_maintenance','channel_switch','ap_maintenance','runtime_power','wmi_notifications'] else []) + ['-Wno-unused-function', '-I' + str(include), '-Isrc/kext', '-Ivendor/itlwm',
                'tests/test_' + name + '.cpp'] + libraries + ['-o', str(executable)])
            run(name, [str(executable)])
    (out / 'summary.json').write_text(json.dumps({
        'passed': passed, 'assertions_enabled': True,
        'full_development_suite': args.only is None, 'selected_tests': names,
        'macos_kext_build': False, 'hardware_validation': False,
        'native_adapter_present': True, 'sae_pmf_implemented': False,
    }, indent=2) + '\n')

if __name__ == '__main__':
    main()
