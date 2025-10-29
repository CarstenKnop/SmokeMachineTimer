# Smoke Machine Diagnostics Console

A lightweight WPF utility for Windows that speaks the Fog Machine debug protocol over a USB serial link. The tool can:

- Enumerate COM ports and open a reliable link to the handheld remote
- Issue debug protocol commands (ping, link statistics, RSSI, channel updates)
- Relay timer metrics retrieved through the remote
- Display live telemetry streams pushed by the remote when a PC client is connected

## Building

```bash
dotnet build PCDiagnostics/DebugConsole/DebugConsole.csproj
```

The project targets .NET 9 (Windows) and uses WPF. Ensure the `System.IO.Ports` package is restored automatically by the build.

## Running

```bash
dotnet run --project PCDiagnostics/DebugConsole/DebugConsole.csproj
```

Select the COM port presented by the remote, press **Connect**, and use the provided actions to query the devices. The log window shows raw protocol activity while the cards on the UI summarize transport statistics for the remote, serial link, and timer.
