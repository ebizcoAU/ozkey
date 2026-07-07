"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import type { ByteArray, TuyaFrame } from "@/lib/tuya";
import { fromHexString } from "@/lib/tuya";
import { useTuyaProtocol, type HardwareMode } from "@/hooks/useTuyaProtocol";
import { useLockState } from "@/hooks/useLockState";
import { useVirtualClock } from "@/hooks/useVirtualClock";
import { useSerialLink } from "@/hooks/useSerialLink";
import { useProvisioning } from "@/hooks/useProvisioning";
import { useMqttLink } from "@/hooks/useMqttLink";
import { useElementWidth } from "@/hooks/useElementWidth";
import {
  ANNOUNCE_TOPIC,
  ONBOARDING_TOPIC,
  buildBroadcastPayload,
  heartbeatTopic,
  lockLogTopic,
  topicMatches,
} from "@/lib/provisioning";
import {
  DEFAULT_BROKER,
  brokerUrl,
  loadBrokerSettings,
  saveBrokerSettings,
  type BrokerSettings,
  type ConversationMessage,
} from "@/lib/broker";
import PhoneShell from "@/components/PhoneShell";
import StatusLeds from "@/components/StatusLeds";
import LockDisplay from "@/components/LockDisplay";
import Keypad from "@/components/Keypad";
import PeripheralControls from "@/components/PeripheralControls";
import KeySlider from "@/components/KeySlider";
import SerialConsole from "@/components/SerialConsole";
import DeviceRegistry from "@/components/DeviceRegistry";
import HardwarePipelineToggle from "@/components/HardwarePipelineToggle";
import ConversationPanel from "@/components/ConversationPanel";
import SettingsDialog from "@/components/SettingsDialog";

const THREE_COLUMN_MIN_WIDTH = 1280;
const MAX_CONVERSATION = 200;

const PAIR_CONFIRM_FILTER = "hotel/locks/+/pair/confirm";

function stamp(): string {
  const d = new Date();
  return (
    [d.getHours(), d.getMinutes(), d.getSeconds()]
      .map((n) => String(n).padStart(2, "0"))
      .join(":") + `.${String(d.getMilliseconds()).padStart(3, "0")}`
  );
}

/** One-line human summary of an MQTT payload for the conversation transcript. */
function summarize(topic: string, payload: string): string {
  try {
    const obj = JSON.parse(payload) as Record<string, unknown>;
    if (obj.payload_hex) return `Command frame (${obj.action ?? "cmd"}) — ${String(obj.payload_hex)}`;
    if (obj.mac && obj.room_no) return `Room assignment → Room ${obj.room_no} (mac ${obj.mac})`;
    if (obj.mac && topic.includes("unpaired")) return `Announce MAC ${obj.mac}`;
    if (topic.includes("heartbeat")) return `Heartbeat${obj.room_no ? ` (room ${obj.room_no})` : ""}`;
    return "JSON message";
  } catch {
    return payload.length > 48 ? `${payload.slice(0, 48)}…` : payload;
  }
}

export default function Page() {
  const clock = useVirtualClock();
  const [mode, setMode] = useState<HardwareMode>("SOFTWARE");
  const { ref: mainRef, width: mainWidth } = useElementWidth<HTMLElement>();

  const [settings, setSettings] = useState<BrokerSettings>(DEFAULT_BROKER);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [conversation, setConversation] = useState<ConversationMessage[]>([]);
  const convSeq = useRef(0);

  // Ref bridges break the hook dependency cycles without re-instantiating hooks.
  const frameHandlerRef = useRef<(f: TuyaFrame) => void>(() => {});
  const onFrame = useCallback((f: TuyaFrame) => frameHandlerRef.current(f), []);
  const receiveBytesRef = useRef<(b: ByteArray) => void>(() => {});
  const onFrameBytes = useCallback((b: ByteArray) => receiveBytesRef.current(b), []);
  const mqttPayloadRef = useRef<(raw: string) => void>(() => {});
  const onMqttPayload = useCallback((raw: string) => mqttPayloadRef.current(raw), []);
  const brokerMessageRef = useRef<(topic: string, payload: string) => void>(() => {});
  const onBrokerMessage = useCallback((t: string, p: string) => brokerMessageRef.current(t, p), []);

  const serial = useSerialLink({ onFrameBytes });
  const protocol = useTuyaProtocol({
    onFrame,
    mode,
    sendToWire: serial.write,
    wireReady: serial.ready,
    onMqttPayload,
  });

  const mqtt = useMqttLink({
    subscriptions: [ONBOARDING_TOPIC, PAIR_CONFIRM_FILTER],
    onMessage: onBrokerMessage,
  });

  const pushConversation = useCallback(
    (dir: "up" | "down", topic: string, summary: string, raw: string, error = false) => {
      const entry: ConversationMessage = { id: ++convSeq.current, time: stamp(), dir, topic, summary, raw, error };
      setConversation((log) => {
        const next = [...log, entry];
        return next.length > MAX_CONVERSATION ? next.slice(-MAX_CONVERSATION) : next;
      });
    },
    []
  );

  const provisioning = useProvisioning({
    mac: settings.mac,
    logInbound: protocol.pushRxLog,
    onEvent: lockPushEventProxy,
  });
  // `lock` is defined below; proxy the event pusher through a ref to avoid TDZ.
  const lockEventRef = useRef<(m: string) => void>(() => {});
  function lockPushEventProxy(m: string) {
    lockEventRef.current(m);
  }

  const provisioningRef = useRef(provisioning.provisioning);
  provisioningRef.current = provisioning.provisioning;

  // Publish the 10-min heartbeat to the room topic so the gateway flushes queued
  // commands (ozkey-02 §3.3 / gap #4).
  const publishHeartbeat = useCallback(() => {
    const p = provisioningRef.current;
    if (!p) return;
    const topic = heartbeatTopic(p.assigned_room_no);
    const payload = JSON.stringify({ mac: p.mac, room_no: p.assigned_room_no, ts: Date.now() });
    if (mqtt.publish(topic, payload)) {
      pushConversation("up", topic, `Heartbeat (room ${p.assigned_room_no})`, payload);
    }
  }, [mqtt, pushConversation]);

  // Push door access transactions up the MAC-scoped usage-log channel so the
  // gateway can persist them (cockpit DOORLOCK LOG tab).
  const publishAccessLog = useCallback(
    (evt: { result: "granted" | "denied" | "expired"; detail: string }) => {
      const p = provisioningRef.current;
      const mac = (p?.mac || settings.mac).toUpperCase();
      const topic = lockLogTopic(mac);
      const payload = JSON.stringify({
        mac,
        room_no: p?.assigned_room_no,
        result: evt.result,
        detail: evt.detail,
        ts: Date.now(),
      });
      if (mqtt.publish(topic, payload)) {
        pushConversation("up", topic, `Access ${evt.result.toUpperCase()} — ${evt.detail}`, payload);
      }
    },
    [mqtt, pushConversation, settings.mac]
  );

  const lock = useLockState({
    transmit: protocol.transmit,
    virtualNow: clock.now,
    onHeartbeat: publishHeartbeat,
    heartbeatSeconds: settings.heartbeatSeconds,
    onAccess: publishAccessLog,
  });
  lockEventRef.current = lock.pushEvent;

  /** Route an inbound JSON payload: command envelope → Tuya parser, else handshake. */
  const handleInboundJson = useCallback(
    (raw: string, topic?: string) => {
      let obj: Record<string, unknown>;
      try {
        obj = JSON.parse(raw) as Record<string, unknown>;
      } catch {
        protocol.pushRxLog(raw.trim() || "(empty)", ["OZKEYSERV payload is not valid JSON"], true);
        return;
      }
      // §3.4 command envelope: unwrap and feed the hex to the Tuya parser (gap #5).
      if (obj.payload_hex) {
        const bytes = fromHexString(String(obj.payload_hex));
        if (bytes) protocol.receiveBytes(bytes);
        else protocol.pushRxLog(raw.trim(), ["command envelope has invalid payload_hex"], true);
        return;
      }
      // §3.2 room-assignment handshake: inject the real topic when not embedded.
      if (obj.mac && obj.room_no) {
        let toValidate = raw;
        if (!obj.topic && topic && topicMatches(ONBOARDING_TOPIC, topic)) {
          toValidate = JSON.stringify({ ...obj, topic });
        }
        provisioning.handleMqttPayload(toValidate);
        return;
      }
      protocol.pushRxLog(raw.trim(), ["unrecognized OZKEYSERV payload (no payload_hex / mac+room_no)"], true);
    },
    [protocol, provisioning]
  );

  // Wire the ref bridges to their real handlers each render.
  frameHandlerRef.current = lock.handleFrame;
  receiveBytesRef.current = protocol.receiveBytes;
  mqttPayloadRef.current = (raw: string) => {
    pushConversation("down", "manual/inject", summarize("manual", raw), raw);
    handleInboundJson(raw, undefined);
  };
  brokerMessageRef.current = (topic: string, payload: string) => {
    pushConversation("down", topic, summarize(topic, payload), payload);
    handleInboundJson(payload, topic);
  };

  // Load persisted broker settings and open the MQTT link once on mount.
  useEffect(() => {
    const loaded = loadBrokerSettings();
    setSettings(loaded);
    void mqtt.connect(loaded);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const applySettings = useCallback(
    (next: BrokerSettings) => {
      setSettings(next);
      saveBrokerSettings(next);
      void mqtt.connect(next);
    },
    [mqtt]
  );

  /** Announce this lock to the broker and enter the awaiting-room state. */
  const registerDoorlock = useCallback(() => {
    const payload = buildBroadcastPayload(settings.mac);
    const ok = mqtt.publish(ANNOUNCE_TOPIC, payload);
    pushConversation("up", ANNOUNCE_TOPIC, `Announce MAC ${settings.mac}`, payload, !ok);
    if (ok) provisioning.beginRegistration();
    else lock.pushEvent("REGISTER FAILED — broker link offline (open System Settings)");
  }, [settings.mac, mqtt, pushConversation, provisioning, lock]);

  const handleModeChange = useCallback(
    (next: HardwareMode) => {
      setMode(next);
      if (next === "SOFTWARE" && serial.ready) void serial.disconnect();
    },
    [serial]
  );

  const threeColumn = mainWidth >= THREE_COLUMN_MIN_WIDTH;

  const conversationPanel = (
    <ConversationPanel
      messages={conversation}
      mqttStatus={mqtt.status}
      brokerUrl={mqtt.url || brokerUrl(settings)}
      registering={provisioning.registering}
      paired={provisioning.provisioning !== null}
      onRegister={registerDoorlock}
      onOpenSettings={() => setSettingsOpen(true)}
      onClear={() => setConversation([])}
    />
  );

  return (
    <main ref={mainRef} className="mx-auto flex min-h-screen max-w-[1900px] flex-col gap-5 p-6 lg:p-10">
      <HardwarePipelineToggle mode={mode} onModeChange={handleModeChange} serial={serial} />

      <div className="flex flex-col items-start justify-center gap-8 lg:flex-row">
        <div className="flex flex-col items-center gap-3">
          <PhoneShell>
            <StatusLeds
              powerState={lock.powerState}
              lowBattery={lock.lowBattery}
              alarm={lock.alarm}
              linkFlashing={provisioning.linkFlashing}
              provisioned={provisioning.provisioning !== null}
              confirmPulse={provisioning.confirmPulse}
            />
            <LockDisplay
              powerState={lock.powerState}
              lockState={lock.lockState}
              pinBuffer={lock.pinBuffer}
              countdown={lock.countdown}
              motorActive={lock.motorActive}
              alarm={lock.alarm}
              lastEvent={lock.lastEvent}
              virtualNowMs={clock.virtualNowMs}
              registering={provisioning.registering}
              pairedBanner={provisioning.pairedBanner}
              roomNo={provisioning.provisioning?.assigned_room_no}
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

        <div className="flex w-full min-w-0 flex-1 flex-col gap-3 lg:mt-2">
          <header>
            <h1 className="text-lg font-bold tracking-tight text-neutral-100">
              LockSim <span className="text-sky-500">·</span> Hardware Testbed
            </h1>
            <p className="text-[11px] text-neutral-500">
              Tuya 0x55 0xAA MCU protocol · Mode A network = MQTT-over-WebSocket to OZKEYSERV
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
          {!threeColumn && conversationPanel}
        </div>

        {threeColumn && (
          <div className="flex w-[440px] shrink-0 flex-col gap-3 lg:mt-2">
            <header>
              <h2 className="text-lg font-bold tracking-tight text-neutral-100">
                Provisioning <span className="text-blue-500">·</span> OZKEYSERV/
              </h2>
              <p className="text-[11px] text-neutral-500">
                Live lock ⇄ server onboarding over the MQTT command pipeline
              </p>
            </header>
            {conversationPanel}
          </div>
        )}
      </div>

      <SettingsDialog
        open={settingsOpen}
        settings={settings}
        mqtt={mqtt}
        onClose={() => setSettingsOpen(false)}
        onSave={applySettings}
      />
    </main>
  );
}
