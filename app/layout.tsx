import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "LockSim — Smart Door Lock Hardware Testbed",
  description:
    "Hardware-accurate smart door lock simulator: Tuya 0x55 0xAA MCU serial protocol over a simulated 4-wire UART (3.3V TTL) bus.",
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en" className="dark">
      <body className="min-h-screen bg-neutral-950 text-neutral-200 antialiased">
        {children}
      </body>
    </html>
  );
}
