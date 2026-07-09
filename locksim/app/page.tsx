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
  deviceIdFromMac,
  heartbeatTopic,
  lockLogTopic,
  ozlockTopic,
  parseOzlockProvision,
  topicMatches,
  type NetworkProvisioning,
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
    if (obj.op === "enrollment_ack") return `OZLOCK enrollment ACK — "${obj.label}" @ site ${obj.site_id}`;
    if (obj.op === "enrollment_nack") return `OZLOCK enrollment REJECTED — ${obj.error}`;
    if (typeof obj.mode === "string" && obj.site_id) return `OZLOCK provision payload (site ${obj.site_id})`;
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
  // Mode A and Mode C both run the SOFTWARE backend; this flag distinguishes
  // them in the toggle so selecting Mode C sticks before enrollment completes.
  const [ozlockSelected, setOzlockSelected] = useState(false);
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

  // Timer-wake heartbeat: room topic in OZKEY mode (ozkey-02 §3.3), device
  // topic in OZLOCK mode (ozkey-04 §9). Both flush the server-side queue.
  const publishHeartbeat = useCallback(() => {
    const p = provisioningRef.current;
    if (!p) return;
    if (p.mode === "ozlock" && p.site_id && p.device_id) {
      const topic = ozlockTopic(p.site_id, p.device_id, "heartbeat");
      const payload = JSON.stringify({ device_id: p.device_id, mac: p.mac, ts: Date.now() });
      if (mqtt.publish(topic, payload)) {
        pushConversation("up", topic, `Heartbeat (${p.label || p.device_id})`, payload);
      }
      return;
    }
    const topic = heartbeatTopic(p.assigned_room_no);
    const payload = JSON.stringify({ mac: p.mac, room_no: p.assigned_room_no, ts: Date.now() });
    if (mqtt.publish(topic, payload)) {
      pushConversation("up", topic, `Heartbeat (room ${p.assigned_room_no})`, payload);
    }
  }, [mqtt, pushConversation]);

  // Push door access transactions up the usage-log channel so the server can
  // persist them (cockpit / OZLOCK app DOORLOCK LOG).
  const publishAccessLog = useCallback(
    (evt: { result: "granted" | "denied" | "expired"; detail: string }) => {
      const p = provisioningRef.current;
      const mac = (p?.mac || settings.mac).toUpperCase();
      const isOzlock = p?.mode === "ozlock" && p.site_id && p.device_id;
      const topic = isOzlock
        ? ozlockTopic(p.site_id!, p.device_id!, "log")
        : lockLogTopic(mac);
      const payload = JSON.stringify({
        device_id: isOzlock ? p.device_id : undefined,
        mac,
        room_no: isOzlock ? undefined : p?.assigned_room_no,
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

  // Pending OZLOCK enrollment context (between provision write and server ack).
  const ozlockPendingRef = useRef<{
    siteId: string;
    deviceId: string;
    appId: string;
    serverIp: string;
  } | null>(null);

  /** ozkey-04 §6 step 3-6: accept the provision payload ("BLE write"), enroll. */
  const startOzlockEnrollment = useCallback(
    (obj: Record<string, unknown>, raw: string) => {
      const parsed = parseOzlockProvision(obj, settings.mac);
      if (!parsed.ok) {
        protocol.pushRxLog(raw.trim(), [`OZLOCK PROVISION REJECTED — ${parsed.error}`], true);
        return;
      }
      // The app granted this device_id + registered the app_id ⇄ device_id pair.
      const { deviceId, siteId, appId } = parsed;
      ozlockPendingRef.current = { siteId, deviceId, appId, serverIp: parsed.serverIp };
      // Subscribe our command topic BEFORE enrolling so the ack can't race us.
      mqtt.subscribe(ozlockTopic(siteId, deviceId, "command"));
      const enrollTopic = ozlockTopic(siteId, deviceId, "enroll");
      const payload = JSON.stringify({
        device_id: deviceId,
        app_id: appId,
        mac: settings.mac,
        // v2: token-free (device_id is the handle); include only if a legacy
        // payload supplied one.
        ...(parsed.enrollmentToken ? { enrollment_token: parsed.enrollmentToken } : {}),
        fw: "1.4.2",
        ts: Date.now(),
      });
      const ok = mqtt.publish(enrollTopic, payload);
      pushConversation("up", enrollTopic, `Enroll ${deviceId} → app ${appId || "(anon)"}`, payload, !ok);
      if (ok) provisioning.beginEnrollment(deviceId, siteId);
      else lock.pushEvent("ENROLL FAILED — broker link offline (open System Settings)");
    },
    [mqtt, protocol, provisioning, pushConversation, settings.mac, lock]
  );

  /**
   * Direct OZLOCK enrollment without a pasted payload (the "we already
   * exchanged the ID" bench model): announce THIS lock's own device_id to
   * OZLOCK using the broker/site it's already configured with. BANOI must have
   * registered this same device_id via POST /pairings first; OZLOCK matches by
   * id. app_id is filled server-side from that registration.
   */
  const enrollOzlockDirect = useCallback(() => {
    const siteId = settings.ozlockSiteId || "lab";
    const deviceId = deviceIdFromMac(settings.mac);
    ozlockPendingRef.current = { siteId, deviceId, appId: "", serverIp: settings.host };
    mqtt.subscribe(ozlockTopic(siteId, deviceId, "command"));
    const enrollTopic = ozlockTopic(siteId, deviceId, "enroll");
    const payload = JSON.stringify({ device_id: deviceId, mac: settings.mac, fw: "1.4.2", ts: Date.now() });
    const ok = mqtt.publish(enrollTopic, payload);
    pushConversation("up", enrollTopic, `Announce ${deviceId} to OZLOCK site '${siteId}'`, payload, !ok);
    if (ok) provisioning.beginEnrollment(deviceId, siteId);
    else lock.pushEvent("ENROLL FAILED — broker link offline (open System Settings)");
  }, [settings.ozlockSiteId, settings.mac, settings.host, mqtt, provisioning, pushConversation, lock]);

  /** ozkey-04 §6 step 8: enrollment ack — persist the OZLOCK network identity. */
  const completeOzlockEnrollment = useCallback(
    (obj: Record<string, unknown>) => {
      const pending = ozlockPendingRef.current;
      const record: NetworkProvisioning = {
        mac: settings.mac,
        assigned_room_no: "",
        server_ip: pending?.serverIp || "",
        mac_token: String(obj.broker_secret || ""),
        provisionedAt: Date.now(),
        mode: "ozlock",
        site_id: String(obj.site_id || pending?.siteId || ""),
        device_id: String(obj.device_id || pending?.deviceId || deviceIdFromMac(settings.mac)),
        app_id: String(obj.app_id || pending?.appId || ""),
        label: typeof obj.label === "string" ? obj.label : undefined,
      };
      ozlockPendingRef.current = null;
      provisioning.adoptProvisioning(record, `ENROLLED — ${record.label || "OZLOCK"}`);
    },
    [provisioning, settings.mac]
  );

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
      // ozkey-04 §5 provision payload (BANOI BLE write, lab: paste/MQTT).
      // v2 (/pairings) is token-free — detect on mode + site_id, NOT the
      // retired enrollment_token.
      if (typeof obj.mode === "string" && obj.site_id) {
        startOzlockEnrollment(obj, raw);
        return;
      }
      // ozkey-04 §6 enrollment outcome from OZLOCK.
      if (obj.op === "enrollment_ack") {
        completeOzlockEnrollment(obj);
        return;
      }
      if (obj.op === "enrollment_nack") {
        protocol.pushRxLog(raw.trim(), [`OZLOCK ENROLLMENT REJECTED — ${obj.error}`], true);
        lock.pushEvent(`ENROLL REJECTED — ${obj.error}`);
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
    [protocol, provisioning, lock, startOzlockEnrollment, completeOzlockEnrollment]
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

  // An enrolled OZLOCK lock listens on its device command topic across
  // reloads/reconnects (dynamic subs survive reconnect inside useMqttLink).
  useEffect(() => {
    const p = provisioning.provisioning;
    if (p?.mode === "ozlock" && p.site_id && p.device_id) {
      mqtt.subscribe(ozlockTopic(p.site_id, p.device_id, "command"));
    }
  }, [provisioning.provisioning, mqtt]);

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

  const ozlockActive =
    ozlockSelected ||
    provisioning.provisioning?.mode === "ozlock" ||
    (provisioning.registering && ozlockPendingRef.current !== null);

  const handleModeChange = useCallback(
    (next: HardwareMode) => {
      setMode(next);
      setOzlockSelected(false); // Mode A / B are not the OZLOCK personal cloud
      if (next === "SOFTWARE" && serial.ready) void serial.disconnect();
      // Selecting Mode A while enrolled with OZLOCK leaves the personal cloud.
      if (next === "SOFTWARE" && provisioningRef.current?.mode === "ozlock") {
        ozlockPendingRef.current = null;
        provisioning.resetProvisioning(
          "LEFT OZLOCK — provisioning wiped, back to OZKEYSERV room pairing (Mode A)"
        );
      }
    },
    [serial, provisioning]
  );

  /** Mode C selected: stick to it, and (if not yet enrolled) explain how. */
  const handleSelectOzlock = useCallback(() => {
    setMode("SOFTWARE");
    setOzlockSelected(true);
    if (serial.ready) void serial.disconnect();
    if (provisioningRef.current?.mode === "ozlock") return;
    lock.pushEvent(
      "MODE C — get a provision payload from the OZLOCK app (:4300 ADD DOORLOCK) and paste it into SERVER PUSH"
    );
  }, [serial, lock]);

  const threeColumn = mainWidth >= THREE_COLUMN_MIN_WIDTH;

  const conversationPanel = (
    <ConversationPanel
      messages={conversation}
      mqttStatus={mqtt.status}
      brokerUrl={mqtt.url || brokerUrl(settings)}
      registering={provisioning.registering}
      paired={provisioning.provisioning !== null}
      ozlockMode={ozlockActive}
      onRegister={registerDoorlock}
      onEnrollOzlock={enrollOzlockDirect}
      onOpenSettings={() => setSettingsOpen(true)}
      onClear={() => setConversation([])}
    />
  );

  return (
    <main ref={mainRef} className="mx-auto flex min-h-screen max-w-[1900px] flex-col gap-5 p-6 lg:p-10">
      <HardwarePipelineToggle
        mode={mode}
        onModeChange={handleModeChange}
        serial={serial}
        ozlockActive={ozlockActive}
        onSelectOzlock={handleSelectOzlock}
      />

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
              roomNo={
                provisioning.provisioning?.mode === "ozlock"
                  ? provisioning.provisioning.label || "OZLOCK"
                  : provisioning.provisioning?.assigned_room_no
              }
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
        provisioning={provisioning.provisioning}
        deviceId={deviceIdFromMac(settings.mac)}
        onClose={() => setSettingsOpen(false)}
        onSave={applySettings}
      />
    </main>
  );
}
