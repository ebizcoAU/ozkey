"use client";

import { useCallback, useRef, useState } from "react";
import type { ByteArray, TuyaFrame } from "@/lib/tuya";
import { useTuyaProtocol, type HardwareMode } from "@/hooks/useTuyaProtocol";
import { useLockState } from "@/hooks/useLockState";
import { useVirtualClock } from "@/hooks/useVirtualClock";
import { useSerialLink } from "@/hooks/useSerialLink";
import PhoneShell from "@/components/PhoneShell";
import StatusLeds from "@/components/StatusLeds";
import LockDisplay from "@/components/LockDisplay";
import Keypad from "@/components/Keypad";
import PeripheralControls from "@/components/PeripheralControls";
import KeySlider from "@/components/KeySlider";
import SerialConsole from "@/components/SerialConsole";
import DeviceRegistry from "@/components/DeviceRegistry";
import HardwarePipelineToggle from "@/components/HardwarePipelineToggle";

export default function Page() {
  const clock = useVirtualClock();
  const [mode, setMode] = useState<HardwareMode>("SOFTWARE");

  // Ref bridges break two dependency cycles without re-instantiating hooks:
  //  - protocol.onFrame -> lock.handleFrame (lock needs protocol.transmit)
  //  - serial read loop -> protocol.receiveBytes (protocol needs serial.write)
  const frameHandlerRef = useRef<(f: TuyaFrame) => void>(() => {});
  const onFrame = useCallback((f: TuyaFrame) => frameHandlerRef.current(f), []);
  const receiveBytesRef = useRef<(b: ByteArray) => void>(() => {});
  const onFrameBytes = useCallback((b: ByteArray) => receiveBytesRef.current(b), []);

  const serial = useSerialLink({ onFrameBytes });
  const protocol = useTuyaProtocol({
    onFrame,
    mode,
    sendToWire: serial.write,
    wireReady: serial.ready,
  });
  const lock = useLockState({ transmit: protocol.transmit, virtualNow: clock.now });
  frameHandlerRef.current = lock.handleFrame;
  receiveBytesRef.current = protocol.receiveBytes;

  // Leaving Mode B releases the physical port so the ESP32 link isn't left open.
  const handleModeChange = useCallback(
    (next: HardwareMode) => {
      setMode(next);
      if (next === "SOFTWARE" && serial.ready) void serial.disconnect();
    },
    [serial]
  );

  return (
    <main className="mx-auto flex min-h-screen max-w-[1500px] flex-col gap-5 p-6 lg:p-10">
      <HardwarePipelineToggle mode={mode} onModeChange={handleModeChange} serial={serial} />

      <div className="flex flex-col items-start justify-center gap-8 lg:flex-row">
      <div className="flex flex-col items-center gap-3">
        <PhoneShell>
          <StatusLeds powerState={lock.powerState} lowBattery={lock.lowBattery} alarm={lock.alarm} />
          <LockDisplay
            powerState={lock.powerState}
            lockState={lock.lockState}
            pinBuffer={lock.pinBuffer}
            countdown={lock.countdown}
            motorActive={lock.motorActive}
            alarm={lock.alarm}
            lastEvent={lock.lastEvent}
            virtualNowMs={clock.virtualNowMs}
          />
          <Keypad onKey={lock.pressKey} />
          <KeySlider engaged={lock.mechanicalKey} onChange={lock.setMechanicalKey} />
          <PeripheralControls
            onTapRfid={lock.tapRfid}
            onScanFingerprint={lock.scanFingerprint}
            onLowBattery={lock.triggerLowBattery}
            lowBattery={lock.lowBattery}
            credentials={lock.credentials}
            virtualNowMs={clock.virtualNowMs}
          />
        </PhoneShell>
        <div className="max-w-[390px] text-center text-[10px] leading-relaxed text-neutral-600">
          Master PIN <span className="text-neutral-400">123456#</span> · Master card UID{" "}
          <span className="text-neutral-400">7B 3F 91 D2</span> · Fingerprint alternates pass/fail
        </div>
      </div>

      <div className="flex w-full flex-col gap-3 lg:mt-2">
        <header>
          <h1 className="text-lg font-bold tracking-tight text-neutral-100">
            LockSim <span className="text-sky-500">·</span> Hardware Testbed
          </h1>
          <p className="text-[11px] text-neutral-500">
            Tuya 0x55 0xAA MCU protocol over simulated 4-wire UART (3.3V TTL) · unencrypted serial bus
          </p>
        </header>
        <SerialConsole
          rxLog={protocol.rxLog}
          txLog={protocol.txLog}
          onInject={protocol.injectHex}
          onServerPush={protocol.serverPush}
          onClear={protocol.clearLogs}
          clock={clock}
          mode={mode}
        />
        <DeviceRegistry
          credentials={lock.credentials}
          virtualNowMs={clock.virtualNowMs}
          onRevoke={lock.revokeCredential}
        />
      </div>
      </div>
    </main>
  );
}
