"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import {
  DEVICE_MAC,
  ONBOARDING_TOPIC,
  buildBroadcastPayload,
  clearProvisioning,
  loadProvisioning,
  parseOnboardingPayload,
  saveProvisioning,
  type NetworkProvisioning,
} from "@/lib/provisioning";

interface UseProvisioningOptions {
  /** Publish a payload up to the OZKEYSERV/ broker (TX log + optional wire). */
  uplink: (payload: string, label: string) => void;
  /** Record an inbound MQTT payload in the RX stream. */
  logInbound: (text: string, notes: string[], error?: boolean) => void;
  /** Surface a one-line status message on the lock's event ticker. */
  onEvent: (message: string) => void;
}

const PAIRED_BANNER_MS = 6000;

/**
 * BLE provisioning state machine. Drives the unprovisioned advertising state,
 * the MAC broadcast, and the OZKEYSERV/ handshake capture that pairs the lock to
 * a room and persists its network identity to LocalStorage.
 */
export function useProvisioning({ uplink, logInbound, onEvent }: UseProvisioningOptions) {
  const [bleMode, setBleModeState] = useState(false);
  const [provisioning, setProvisioning] = useState<NetworkProvisioning | null>(null);
  const [pairedBanner, setPairedBanner] = useState<string | null>(null);
  // Incremented on each successful registration to retrigger the LED green-x3 pulse.
  const [confirmPulse, setConfirmPulse] = useState(0);

  const bannerTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const onEventRef = useRef(onEvent);
  onEventRef.current = onEvent;
  const uplinkRef = useRef(uplink);
  uplinkRef.current = uplink;
  const logInboundRef = useRef(logInbound);
  logInboundRef.current = logInbound;

  useEffect(() => {
    setProvisioning(loadProvisioning());
  }, []);

  /** Toggle BLE Provisioning Mode. Turning on wipes the existing pairing. */
  const setBleMode = useCallback((on: boolean) => {
    setBleModeState(on);
    if (on) {
      clearProvisioning();
      setProvisioning(null);
      setPairedBanner(null);
      onEventRef.current(`BLE PROVISIONING ON — DEVICE UNPROVISIONED (MAC ${DEVICE_MAC})`);
    } else {
      onEventRef.current("BLE PROVISIONING OFF");
    }
  }, []);

  /** Advertise the hardware MAC up to the OZKEYSERV/ broker layer. */
  const broadcastMac = useCallback(() => {
    const payload = buildBroadcastPayload(DEVICE_MAC);
    uplinkRef.current(payload, `BLE ADV → OZKEYSERV/ (MAC ${DEVICE_MAC})`);
    onEventRef.current(`BROADCAST MAC ${DEVICE_MAC} → OZKEYSERV/ broker`);
  }, []);

  /**
   * Capture an inbound MQTT payload. A structurally valid onboarding handshake
   * for this device halts BLE advertising, persists the network variables, and
   * fires the green registration confirmation.
   */
  const handleMqttPayload = useCallback((raw: string) => {
    const result = parseOnboardingPayload(raw, DEVICE_MAC);
    if (!result.ok) {
      logInboundRef.current(raw.trim(), [`OZKEYSERV ONBOARDING REJECTED — ${result.error}`], true);
      return;
    }

    logInboundRef.current(raw.trim(), [
      `MQTT ${result.topic} (matches ${ONBOARDING_TOPIC})`,
      `Onboarding validated — MAC ${result.mac} → Room ${result.roomNo}`,
      `Network vars: server_ip=${result.serverIp}, mac_token=${result.macToken}`,
    ]);

    const record: NetworkProvisioning = {
      mac: result.mac,
      assigned_room_no: result.roomNo,
      server_ip: result.serverIp,
      mac_token: result.macToken,
      provisionedAt: Date.now(),
    };
    saveProvisioning(record);
    setProvisioning(record);

    setBleModeState(false); // halt the BLE blue flashing loop instantly
    setConfirmPulse((n) => n + 1); // LED green x3 confirmation

    const banner = `PAIRED - ROOM ${result.roomNo}`;
    setPairedBanner(banner);
    onEventRef.current(`NETWORK REGISTERED — ${banner} @ ${result.serverIp} (token ${result.macToken})`);
    if (bannerTimer.current) clearTimeout(bannerTimer.current);
    bannerTimer.current = setTimeout(() => setPairedBanner(null), PAIRED_BANNER_MS);
  }, []);

  useEffect(
    () => () => {
      if (bannerTimer.current) clearTimeout(bannerTimer.current);
    },
    []
  );

  const bleFlashing = bleMode && pairedBanner === null;

  return {
    bleMode,
    provisioning,
    pairedBanner,
    confirmPulse,
    bleFlashing,
    deviceMac: DEVICE_MAC,
    setBleMode,
    broadcastMac,
    handleMqttPayload,
  };
}

export type ProvisioningApi = ReturnType<typeof useProvisioning>;
