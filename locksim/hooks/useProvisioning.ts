"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import {
  ONBOARDING_TOPIC,
  clearProvisioning,
  loadProvisioning,
  parseOnboardingPayload,
  saveProvisioning,
  type NetworkProvisioning,
} from "@/lib/provisioning";

interface UseProvisioningOptions {
  /** This lock's hardware MAC (from broker settings). */
  mac: string;
  /** Record an inbound handshake payload in the RX stream. */
  logInbound: (text: string, notes: string[], error?: boolean) => void;
  /** Surface a one-line status message on the lock's event ticker. */
  onEvent: (message: string) => void;
}

const PAIRED_BANNER_MS = 6000;

/**
 * Provisioning state machine. Tracks registration (announce → awaiting room),
 * captures the OZKEYSERV room-assignment handshake, persists the network
 * identity, and drives the LED green-x3 confirmation on a successful pair.
 * Transport-agnostic: registration announces are published by the caller (MQTT),
 * this hook only owns state + handshake validation.
 */
export function useProvisioning({ mac, logInbound, onEvent }: UseProvisioningOptions) {
  const [registering, setRegistering] = useState(false);
  const [provisioning, setProvisioning] = useState<NetworkProvisioning | null>(null);
  const [pairedBanner, setPairedBanner] = useState<string | null>(null);
  // Incremented on each successful registration to retrigger the LED green-x3 pulse.
  const [confirmPulse, setConfirmPulse] = useState(0);

  const bannerTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const macRef = useRef(mac);
  macRef.current = mac;
  const onEventRef = useRef(onEvent);
  onEventRef.current = onEvent;
  const logInboundRef = useRef(logInbound);
  logInboundRef.current = logInbound;

  useEffect(() => {
    setProvisioning(loadProvisioning());
  }, []);

  /** Enter the unprovisioned "awaiting room" state (wipes any prior pairing). */
  const beginRegistration = useCallback(() => {
    clearProvisioning();
    setProvisioning(null);
    setPairedBanner(null);
    setRegistering(true);
    onEventRef.current(`REGISTERING — announced MAC ${macRef.current}, awaiting room assignment`);
  }, []);

  /**
   * Adopt a provisioning record produced outside the room-handshake parser
   * (OZLOCK enrollment ack, ozkey-04 §6). Persists, confirms, banners.
   */
  const adoptProvisioning = useCallback((record: NetworkProvisioning, banner: string) => {
    saveProvisioning(record);
    setProvisioning(record);
    setRegistering(false);
    setConfirmPulse((n) => n + 1);
    setPairedBanner(banner);
    onEventRef.current(`NETWORK REGISTERED — ${banner}`);
    if (bannerTimer.current) clearTimeout(bannerTimer.current);
    bannerTimer.current = setTimeout(() => setPairedBanner(null), PAIRED_BANNER_MS);
  }, []);

  /** Enter the awaiting-enrollment state (OZLOCK mode; wipes prior pairing). */
  const beginEnrollment = useCallback((deviceId: string, siteId: string) => {
    clearProvisioning();
    setProvisioning(null);
    setPairedBanner(null);
    setRegistering(true);
    onEventRef.current(`ENROLLING — ${deviceId} → OZLOCK site '${siteId}', awaiting ack`);
  }, []);

  /** Wipe provisioning without entering a registering state (mode switch). */
  const resetProvisioning = useCallback((reason: string) => {
    clearProvisioning();
    setProvisioning(null);
    setPairedBanner(null);
    setRegistering(false);
    onEventRef.current(reason);
  }, []);

  /**
   * Capture an inbound room-assignment handshake. A structurally valid payload
   * for this device persists the network variables, stops the registering
   * state, and fires the green registration confirmation.
   */
  const handleMqttPayload = useCallback((raw: string) => {
    const result = parseOnboardingPayload(raw, macRef.current);
    if (!result.ok) {
      logInboundRef.current(raw.trim(), [`ONBOARDING REJECTED — ${result.error}`], true);
      return;
    }

    logInboundRef.current(raw.trim(), [
      `${result.topic} (matches ${ONBOARDING_TOPIC})`,
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
    setRegistering(false);
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

  return {
    registering,
    provisioning,
    pairedBanner,
    confirmPulse,
    linkFlashing: registering && pairedBanner === null,
    beginRegistration,
    beginEnrollment,
    adoptProvisioning,
    resetProvisioning,
    handleMqttPayload,
  };
}

export type ProvisioningApi = ReturnType<typeof useProvisioning>;
