import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "Industrial Sensor Node — Digital Twin",
  description: "Real-time telemetry dashboard for ESP32-S3 sensor nodes",
};

export default function RootLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
