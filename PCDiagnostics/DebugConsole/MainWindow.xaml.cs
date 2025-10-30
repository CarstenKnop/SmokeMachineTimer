using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO.Ports;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media.Imaging;
using System.Globalization;
using SmokeMachineDiagnostics.Protocol;
using SmokeMachineDiagnostics.Services;

namespace SmokeMachineDiagnostics;

public partial class MainWindow : Window
{
    private readonly SerialClient _client = new();
    private readonly CancellationTokenSource _uiCts = new();
    private readonly ObservableCollection<TimerListItem> _timers = new();
    private readonly Dictionary<byte, TimerListItem> _timerMap = new();
    private readonly ObservableCollection<DiscoveredListItem> _discovered = new();
    private readonly Dictionary<string, DiscoveredListItem> _discoveredMap = new(StringComparer.OrdinalIgnoreCase);
    private readonly HeatBarPlot _remotePlot;
    private readonly HeatBarPlot _timerPlot;
    private CancellationTokenSource? _scanCts;
    private CancellationTokenSource? _discoveryCts;
    private byte _lastKnownChannel = 1;
    private byte _minSampledChannel = 255;
    private byte _maxSampledChannel = 0;
    private readonly int[] _remoteSampleCounts = new int[13];
    private readonly int[] _timerSampleCounts = new int[13];
    private readonly sbyte[] _remoteLastRssi = Enumerable.Repeat<sbyte>(0, 13).ToArray();
    private readonly sbyte[] _timerLastRssi = Enumerable.Repeat<sbyte>(0, 13).ToArray();
    private readonly bool[] _remoteSampleFaultLogged = new bool[13];
    private readonly bool[] _timerSampleFaultLogged = new bool[13];

    public MainWindow()
    {
        InitializeComponent();
    _remotePlot = new HeatBarPlot(364, 240);
    _timerPlot = new HeatBarPlot(364, 240);
        RemoteGraphImage.Source = _remotePlot.Bitmap;
        TimerGraphImage.Source = _timerPlot.Bitmap;
        TimersList.ItemsSource = _timers;
        DiscoveredList.ItemsSource = _discovered;
        _client.PacketReceived += OnPacketReceived;
        _client.Log += AppendLog;
        Loaded += (_, _) => RefreshPortList();
        Closing += (_, _) =>
        {
            _scanCts?.Cancel();
            _discoveryCts?.Cancel();
            _uiCts.Cancel();
            _client.Dispose();
        };
        UpdateDiscoveryControls();
        UpdateHeatmapAxes();
    }

    private void RefreshPorts_OnClick(object sender, RoutedEventArgs e) => RefreshPortList();

    private void RefreshPortList()
    {
        string? selected = PortSelector.SelectedItem as string;
        PortSelector.ItemsSource = SerialPort.GetPortNames();
        if (selected != null)
        {
            PortSelector.SelectedItem = selected;
        }
        UpdateLocalStats();
    }

    private async void ConnectButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (!_client.IsConnected)
        {
            string? portName = PortSelector.SelectedItem as string;
            if (string.IsNullOrWhiteSpace(portName))
            {
                AppendLog("Select a COM port first.");
                return;
            }
            try
            {
                await _client.ConnectAsync(portName, 115200, _uiCts.Token);
                ConnectButton.Content = "Disconnect";
                await FetchDeviceInventoryAsync();
                await FetchDiscoveredAsync();
            }
            catch (Exception ex)
            {
                AppendLog($"Connect failed: {ex.Message}");
            }
        }
        else
        {
            _scanCts?.Cancel();
            _discoveryCts?.Cancel();
            _discoveryCts = null;
            _discoveredMap.Clear();
            _discovered.Clear();
            _client.Disconnect();
            ConnectButton.Content = "Connect";
        }
        UpdateLocalStats();
        UpdateDiscoveryControls();
    }

    private async void PingButton_OnClick(object sender, RoutedEventArgs e) =>
        await ExecuteCommandAsync(DebugProtocol.Command.Ping, Array.Empty<byte>(), "Ping");

    private async void GetRemoteStatsButton_OnClick(object sender, RoutedEventArgs e) =>
        await ExecuteCommandAsync(DebugProtocol.Command.GetRemoteStats, Array.Empty<byte>(), "GetRemoteStats");

    private async void GetTimerStatsButton_OnClick(object sender, RoutedEventArgs e) =>
        await ExecuteCommandAsync(DebugProtocol.Command.GetTimerStats, Array.Empty<byte>(), "GetTimerStats");

    private async void SetChannelButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (!byte.TryParse(ChannelInput.Text, out byte channel) || channel < 1 || channel > 13)
        {
            AppendLog("Channel must be between 1 and 13.");
            return;
        }
        byte[] payload = { channel, InformTimerCheck.IsChecked == true ? (byte)1 : (byte)0 };
        await ExecuteCommandAsync(DebugProtocol.Command.SetChannel, payload, "SetChannel");
    }

    private async void LoadTimersButton_OnClick(object sender, RoutedEventArgs e) => await FetchDeviceInventoryAsync();

    private async void StartDiscoveryButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (!_client.IsConnected)
        {
            AppendLog("Connect before starting discovery.");
            return;
        }
        if (_discoveryCts != null)
        {
            AppendLog("Discovery is already running.");
            return;
        }
        try
        {
            var response = await _client.SendAsync(DebugProtocol.Command.StartDiscovery, Array.Empty<byte>(), _uiCts.Token);
            ProcessResponse(response);
            _discoveredMap.Clear();
            _discovered.Clear();
            _discoveryCts = new CancellationTokenSource();
            _ = Task.Run(() => PollDiscoveryLoopAsync(_discoveryCts.Token));
        }
        catch (Exception ex)
        {
            AppendLog($"Start discovery failed: {ex.Message}");
        }
        UpdateDiscoveryControls();
    }

    private async void StopDiscoveryButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (!_client.IsConnected)
        {
            AppendLog("Connect before stopping discovery.");
            return;
        }
        _discoveryCts?.Cancel();
        _discoveryCts = null;
        try
        {
            var response = await _client.SendAsync(DebugProtocol.Command.StopDiscovery, Array.Empty<byte>(), _uiCts.Token);
            ProcessResponse(response);
            await FetchDiscoveredAsync();
        }
        catch (Exception ex)
        {
            AppendLog($"Stop discovery failed: {ex.Message}");
        }
        UpdateDiscoveryControls();
    }

    private async void RefreshDiscoveryButton_OnClick(object sender, RoutedEventArgs e)
    {
        await FetchDiscoveredAsync();
        UpdateDiscoveryControls();
    }

    private void DiscoveredList_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (DiscoveredList.SelectedItem is DiscoveredListItem item)
        {
            string baseName = string.IsNullOrWhiteSpace(item.RemoteName) ? item.TimerName : item.RemoteName;
            RenameInput.Text = baseName;
        }
        else
        {
            RenameInput.Text = string.Empty;
        }
        UpdateDiscoverySelectionButtons();
    }

    private async void PairButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (DiscoveredList.SelectedItem is not DiscoveredListItem item)
        {
            return;
        }
        if (!_client.IsConnected)
        {
            AppendLog("Connect before pairing.");
            return;
        }
        if (item.IsPaired)
        {
            AppendLog("Timer is already paired.");
            return;
        }
        try
        {
            byte[] payload = { item.DiscoveryIndex };
            var response = await _client.SendAsync(DebugProtocol.Command.PairDiscoveredDevice, payload, _uiCts.Token);
            ProcessResponse(response);
            await FetchDeviceInventoryAsync();
            await FetchDiscoveredAsync();
        }
        catch (Exception ex)
        {
            AppendLog($"Pair failed: {ex.Message}");
        }
        UpdateDiscoveryControls();
    }

    private async void UnpairButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (DiscoveredList.SelectedItem is not DiscoveredListItem item)
        {
            return;
        }
        if (!_client.IsConnected)
        {
            AppendLog("Connect before unpairing.");
            return;
        }
        if (!item.IsPaired)
        {
            AppendLog("Timer is not paired.");
            return;
        }
        try
        {
            byte[] payload = { item.PairedIndex };
            var response = await _client.SendAsync(DebugProtocol.Command.UnpairDevice, payload, _uiCts.Token);
            ProcessResponse(response);
            await FetchDeviceInventoryAsync();
            await FetchDiscoveredAsync();
        }
        catch (Exception ex)
        {
            AppendLog($"Unpair failed: {ex.Message}");
        }
        UpdateDiscoveryControls();
    }

    private async void RenameButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (DiscoveredList.SelectedItem is not DiscoveredListItem item)
        {
            AppendLog("Select a timer to rename.");
            return;
        }
        if (!_client.IsConnected)
        {
            AppendLog("Connect before renaming a timer.");
            return;
        }
        if (!item.IsPaired)
        {
            AppendLog("Only paired timers can be renamed.");
            return;
        }
        string trimmed = RenameInput.Text.Trim();
        if (string.IsNullOrEmpty(trimmed))
        {
            AppendLog("Enter a name before renaming.");
            return;
        }
        try
        {
            byte[] payload = new byte[1 + 10];
            payload[0] = item.PairedIndex;
            int written = Encoding.ASCII.GetBytes(trimmed, 0, Math.Min(trimmed.Length, 10), payload, 1);
            for (int i = 1 + written; i < payload.Length; i++)
            {
                payload[i] = 0;
            }
            var response = await _client.SendAsync(DebugProtocol.Command.RenameDevice, payload, _uiCts.Token);
            ProcessResponse(response);
            await FetchDeviceInventoryAsync();
            await FetchDiscoveredAsync();
        }
        catch (Exception ex)
        {
            AppendLog($"Rename failed: {ex.Message}");
        }
        UpdateDiscoveryControls();
    }

    private async void StartScanButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (!_client.IsConnected)
        {
            AppendLog("Connect before starting a scan.");
            return;
        }
        if (_scanCts != null)
        {
            AppendLog("A scan is already running.");
            return;
        }
        var selected = _timers.Where(t => t.IsSelected).ToList();
        if (selected.Count == 0)
        {
            AppendLog("Select at least one timer to scan.");
            return;
        }
        if (!int.TryParse(SamplesPerChannelInput.Text, out int samplesPerChannel) || samplesPerChannel <= 0)
        {
            AppendLog("Samples per channel must be positive.");
            return;
        }
        if (!int.TryParse(SettleDelayInput.Text, out int settleMs) || settleMs < 0)
        {
            AppendLog("Settle delay must be zero or greater.");
            return;
        }
        if (!int.TryParse(SampleDelayInput.Text, out int sampleMs) || sampleMs < 0)
        {
            AppendLog("Sample delay must be zero or greater.");
            return;
        }
        byte originalChannel = _lastKnownChannel;
        try
        {
            var response = await _client.SendAsync(DebugProtocol.Command.GetRemoteStats, Array.Empty<byte>(), _uiCts.Token);
            var payload = ExtractStruct<DebugProtocol.RemoteStatsPayload>(response);
            if (payload != null && payload.Value.RemoteLink.Channel >= 1 && payload.Value.RemoteLink.Channel <= 13)
            {
                originalChannel = payload.Value.RemoteLink.Channel;
            }
        }
        catch (Exception ex)
        {
            AppendLog($"Warning: unable to read current channel ({ex.Message}). Using cached value {originalChannel}.");
        }

        var sessions = selected.Select(t => new ScannerSession(t, samplesPerChannel, settleMs, sampleMs)).ToList();
        _remotePlot.Clear();
        _timerPlot.Clear();
        Array.Clear(_remoteSampleCounts, 0, _remoteSampleCounts.Length);
        Array.Clear(_timerSampleCounts, 0, _timerSampleCounts.Length);
        Array.Clear(_remoteLastRssi, 0, _remoteLastRssi.Length);
        Array.Clear(_timerLastRssi, 0, _timerLastRssi.Length);
        Array.Clear(_remoteSampleFaultLogged, 0, _remoteSampleFaultLogged.Length);
        Array.Clear(_timerSampleFaultLogged, 0, _timerSampleFaultLogged.Length);
        _minSampledChannel = 255;
        _maxSampledChannel = 0;
        _remotePlot.SetDebugInfo(_remoteSampleCounts, _remoteLastRssi);
        _timerPlot.SetDebugInfo(_timerSampleCounts, _timerLastRssi);
        UpdateHeatmapAxes();
        ScanStatusText.Text = "Starting scan...";
        CycleStatusText.Text = string.Empty;
        StartScanButton.IsEnabled = false;
        StopScanButton.IsEnabled = true;
        _scanCts = new CancellationTokenSource();
        _ = Task.Run(() => RunScannerLoopAsync(originalChannel, sessions, _scanCts.Token));
    }

    private void StopScanButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_scanCts != null)
        {
            ScanStatusText.Text = "Stopping...";
            _scanCts.Cancel();
        }
    }

    private async Task ExecuteCommandAsync(DebugProtocol.Command command, ReadOnlyMemory<byte> payload, string label)
    {
        if (!_client.IsConnected)
        {
            AppendLog("Connect the serial client first.");
            return;
        }
        try
        {
            byte[] payloadCopy = payload.ToArray();
            var response = await _client.SendAsync(command, payloadCopy, _uiCts.Token);
            AppendLog($"{label} -> {DebugProtocol.DescribeStatus(response.Status)} (flags={response.Flags})");
            ProcessResponse(response);
        }
        catch (Exception ex)
        {
            AppendLog($"{label} failed: {ex.Message}");
        }
        UpdateLocalStats();
    }

    private void ProcessResponse(in DebugProtocol.Packet packet)
    {
        if (packet.Status != DebugProtocol.Status.Ok)
        {
            AppendLog($"Device status: {DebugProtocol.DescribeStatus(packet.Status)}");
        }

        switch (packet.Command)
        {
            case DebugProtocol.Command.GetRemoteStats:
                HandleRemoteStats(packet);
                break;
            case DebugProtocol.Command.GetTimerStats:
                HandleTimerStats(packet);
                break;
            case DebugProtocol.Command.GetRssi:
                HandleRssiReport(packet);
                break;
            case DebugProtocol.Command.Ping:
                AppendLog("Ping response received.");
                break;
            case DebugProtocol.Command.GetDeviceInventory:
                HandleDeviceInventory(packet);
                break;
            case DebugProtocol.Command.GetDiscoveredDevices:
                HandleDiscoveredDevices(packet);
                break;
            case DebugProtocol.Command.StartDiscovery:
                UpdateDiscoveryStatus();
                break;
            case DebugProtocol.Command.StopDiscovery:
                UpdateDiscoveryStatus();
                break;
            case DebugProtocol.Command.PairDiscoveredDevice:
            case DebugProtocol.Command.UnpairDevice:
            case DebugProtocol.Command.RenameDevice:
                UpdateDiscoveryStatus();
                break;
        }
    }

    private void OnPacketReceived(DebugProtocol.Packet packet)
    {
        Dispatcher.Invoke(() =>
        {
            if (packet.Flags.HasFlag(DebugProtocol.PacketFlags.Streaming))
            {
                if (packet.Command == DebugProtocol.Command.GetRemoteStats)
                {
                    HandleRemoteStats(packet);
                }
            }
            else
            {
                AppendLog($"Event: {DebugProtocol.DescribeCommand(packet.Command)}");
                ProcessResponse(packet);
            }
            UpdateLocalStats();
        });
    }

    private void HandleRemoteStats(DebugProtocol.Packet packet)
    {
        var payload = ExtractStruct<DebugProtocol.RemoteStatsPayload>(packet);
        if (payload == null) return;
        var stats = payload.Value;
        var link = stats.RemoteLink;
        RemoteTransportStats.Text = FormatStats(link.Transport) +
                                    $" | RSSI local={link.RssiLocal} peer={link.RssiPeer} ch={link.Channel}";
        SerialTransportStats.Text = FormatSerialSummary(stats.SerialLink);
        RemoteSnapshotText.Text = FormatSnapshot(stats.Remote);
        if (link.Channel >= 1 && link.Channel <= 13)
        {
            _lastKnownChannel = link.Channel;
            if (!ChannelInput.IsFocused)
            {
                ChannelInput.Text = link.Channel.ToString();
            }
        }
    }

    private void HandleTimerStats(DebugProtocol.Packet packet)
    {
        var payload = ExtractStruct<DebugProtocol.TimerStatsPayload>(packet);
        if (payload == null) return;
        var stats = payload.Value;
        var link = stats.Link;
        TimerTransportStats.Text = FormatStats(link.Transport) +
                                   $" | RSSI local={link.RssiLocal} remote={link.RssiPeer} ch={link.Channel}";
        TimerRssi.Text = $"Timer RSSI local={link.RssiLocal} remote={link.RssiPeer}";
        TimerSnapshotText.Text = FormatSnapshot(stats.Timer);
        RemoteSnapshotText.Text = FormatSnapshot(stats.Remote);

        bool updated = false;
        if (stats.Timer.Channel >= 1 && stats.Timer.Channel <= 13)
        {
            foreach (var item in _timers.Where(t => t.Channel == stats.Timer.Channel))
            {
                item.UpdateSnapshot(stats.Timer);
                updated = true;
            }
        }
        if (!updated)
        {
            foreach (var item in _timers.Where(t => t.IsActive))
            {
                item.UpdateSnapshot(stats.Timer);
            }
        }
        UpdateTimerControlState();
    }

    private void HandleRssiReport(DebugProtocol.Packet packet)
    {
        byte[] payload = CopyPayload(packet);
        if (payload.Length >= 3)
        {
            TimerRssi.Text = $"Remote RSSI local={unchecked((sbyte)payload[0])} timer={unchecked((sbyte)payload[1])} timerLocal={unchecked((sbyte)payload[2])}";
        }
    }

    private void HandleDeviceInventory(DebugProtocol.Packet packet)
    {
        var batch = ParseInventory(packet);
        if (batch == null)
        {
            AppendLog("Inventory payload malformed.");
            return;
        }
        bool isFirst = batch.Value.BatchStart == 0;
        bool isFinal = batch.Value.BatchStart + batch.Value.BatchCount >= batch.Value.TotalCount || batch.Value.BatchCount == 0;
        ApplyInventoryBatch(batch.Value, isFirst, isFinal);
        if (isFinal)
        {
            RebuildTimerList();
        }
    }

    private async Task FetchDeviceInventoryAsync()
    {
        if (!_client.IsConnected)
        {
            AppendLog("Connect first to load timers.");
            return;
        }
        byte nextIndex = 0;
        byte total = byte.MaxValue;
        bool first = true;
        try
        {
            while (nextIndex < total)
            {
                byte[] payload = { nextIndex };
                var packet = await _client.SendAsync(DebugProtocol.Command.GetDeviceInventory, payload, _uiCts.Token).ConfigureAwait(false);
                var batch = ParseInventory(packet);
                if (batch == null)
                {
                    AppendLog("Inventory payload malformed.");
                    break;
                }
                total = batch.Value.TotalCount;
                bool isFinal = batch.Value.BatchStart + batch.Value.BatchCount >= total || batch.Value.BatchCount == 0;
                Dispatcher.Invoke(() =>
                {
                    ApplyInventoryBatch(batch.Value, first, isFinal);
                    if (isFinal)
                    {
                        RebuildTimerList();
                    }
                });
                if (batch.Value.BatchCount == 0)
                {
                    break;
                }
                nextIndex = (byte)(batch.Value.BatchStart + batch.Value.BatchCount);
                first = false;
            }
        }
        catch (Exception ex)
        {
            AppendLog($"GetDeviceInventory failed: {ex.Message}");
        }
    }

    private async Task FetchDiscoveredAsync()
    {
        if (!_client.IsConnected)
        {
            return;
        }
        byte nextIndex = 0;
        byte total = byte.MaxValue;
        bool first = true;
        try
        {
            while (nextIndex < total)
            {
                byte[] payload = { nextIndex };
                var packet = await _client.SendAsync(DebugProtocol.Command.GetDiscoveredDevices, payload, _uiCts.Token).ConfigureAwait(false);
                var batch = ParseDiscovery(packet);
                if (!batch.HasValue)
                {
                    AppendLog("Discovery payload malformed.");
                    break;
                }
                total = batch.Value.TotalCount;
                bool isFinal = batch.Value.BatchStart + batch.Value.BatchCount >= total || batch.Value.BatchCount == 0;
                Dispatcher.Invoke(() =>
                {
                    ApplyDiscoveryBatch(batch.Value, first, isFinal);
                });
                if (batch.Value.BatchCount == 0)
                {
                    break;
                }
                nextIndex = (byte)(batch.Value.BatchStart + batch.Value.BatchCount);
                first = false;
            }
        }
        catch (OperationCanceledException)
        {
            // expected during shutdown or disconnect
        }
        catch (Exception ex)
        {
            AppendLog($"GetDiscoveredDevices failed: {ex.Message}");
        }
    }

    private async Task PollDiscoveryLoopAsync(CancellationToken token)
    {
        try
        {
            while (!token.IsCancellationRequested)
            {
                await FetchDiscoveredAsync().ConfigureAwait(false);
                await Task.Delay(1000, token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException)
        {
            // expected when cancellation requested
        }
        catch (Exception ex)
        {
            AppendLog($"Discovery poll error: {ex.Message}");
        }
    }

    private void UpdateLocalStats()
    {
        if (!_client.IsConnected)
        {
            var stats = _client.Stats;
            SerialTransportStats.Text = FormatStats(stats);
        }
        UpdateDiscoveryControls();
    }

    private void AppendLog(string message)
    {
        Dispatcher.Invoke(() =>
        {
            LogOutput.AppendText($"[{DateTime.Now:HH:mm:ss}] {message}{Environment.NewLine}");
            LogOutput.ScrollToEnd();
        });
    }

    private static string FormatStats(ReliableProtocol.TransportStats stats)
    {
        return $"TX:{stats.TxFrames} ack:{stats.TxAcked} nak:{stats.TxNak} timeout:{stats.TxTimeout} retries:{stats.TxRetries} err:{stats.TxSendErrors} | RX:{stats.RxFrames} ackReq:{stats.RxAckRequests} ackSent:{stats.RxAckSent} nakSent:{stats.RxNakSent} crc:{stats.RxCrcErrors} invalid:{stats.RxInvalidLength} decl:{stats.HandlerDeclined}";
    }

    private static string FormatSerialSummary(DebugProtocol.SerialLinkSummary summary)
    {
        return $"Serial TX:{summary.TxFrames} RX:{summary.RxFrames} err:{summary.Errors} lastStatus:{summary.LastStatusCode}";
    }

    private static string FormatSnapshot(DebugProtocol.TimerSnapshot snapshot)
    {
        return $"ch{snapshot.Channel} TON={snapshot.TonSeconds:F1}s TOFF={snapshot.ToffSeconds:F1}s elapsed={snapshot.ElapsedSeconds:F1}s output={(snapshot.OutputOn != 0 ? "ON" : "OFF")} override={(snapshot.OverrideActive != 0 ? "YES" : "no")}";
    }

    private static T? ExtractStruct<T>(in DebugProtocol.Packet packet) where T : struct
    {
        int size = Marshal.SizeOf<T>();
        if (packet.DataLength < size)
        {
            return null;
        }
        byte[] payload = CopyPayload(packet);
        return MemoryMarshal.Read<T>(payload);
    }

    private static byte[] CopyPayload(in DebugProtocol.Packet packet)
    {
        byte[] data = new byte[packet.DataLength];
        unsafe
        {
            fixed (byte* src = packet.Data)
            {
                new ReadOnlySpan<byte>(src, packet.DataLength).CopyTo(data);
            }
        }
        return data;
    }

    private static InventoryBatch? ParseInventory(in DebugProtocol.Packet packet)
    {
        int headerSize = Marshal.SizeOf<DeviceInventoryHeader>();
        if (packet.DataLength < headerSize)
        {
            return null;
        }
        byte[] payload = CopyPayload(packet);
        var header = MemoryMarshal.Read<DeviceInventoryHeader>(payload.AsSpan(0, headerSize));
        var entries = new List<InventoryEntry>(header.BatchCount);
        int entrySize = DebugProtocol.InventoryEntrySize;
        int offset = headerSize;
        for (int i = 0; i < header.BatchCount; i++)
        {
            if (offset + entrySize > payload.Length)
            {
                return null;
            }
            var raw = MemoryMarshal.Read<DeviceInventoryEntryRaw>(payload.AsSpan(offset, entrySize));
            string mac;
            string name;
            unsafe
            {
                mac = TimerListItem.FormatMac(new ReadOnlySpan<byte>(raw.Mac, 6));
                name = Encoding.ASCII.GetString(new ReadOnlySpan<byte>(raw.Name, 10)).Trim('\0', ' ');
            }
            entries.Add(new InventoryEntry(raw.Index, raw.Channel, name, mac));
            offset += entrySize;
        }
        return new InventoryBatch(header.TotalCount, header.BatchStart, header.BatchCount, header.ActiveIndex, entries);
    }

    private void ApplyInventoryBatch(InventoryBatch batch, bool isFirstBatch, bool isFinalBatch)
    {
        if (isFirstBatch)
        {
            _timerMap.Clear();
            _timers.Clear();
        }
        foreach (var entry in batch.Entries)
        {
            if (!_timerMap.TryGetValue(entry.Index, out var item))
            {
                item = new TimerListItem(entry.Index);
                _timerMap[entry.Index] = item;
                _timers.Add(item);
            }
            item.Apply(entry.Channel, entry.Name, entry.Mac, batch.ActiveIndex == entry.Index);
        }
        if (isFinalBatch)
        {
            UpdateSelectionSummary();
        }
    }

    private void RebuildTimerList()
    {
        var ordered = _timerMap.Keys.OrderBy(k => k).Select(k => _timerMap[k]).ToList();
        _timers.Clear();
        foreach (var item in ordered)
        {
            _timers.Add(item);
        }
        UpdateSelectionSummary();
    }

    private DiscoveryBatch? ParseDiscovery(in DebugProtocol.Packet packet)
    {
        int headerSize = Marshal.SizeOf<DiscoveryHeader>();
        if (packet.DataLength < headerSize)
        {
            return null;
        }
        byte[] payload = CopyPayload(packet);
        var header = MemoryMarshal.Read<DiscoveryHeader>(payload.AsSpan(0, headerSize));
        var entries = new List<DiscoveryEntry>(header.BatchCount);
        int offset = headerSize;
        int entrySize = DebugProtocol.DiscoveryEntrySize;
        for (int i = 0; i < header.BatchCount; i++)
        {
            if (offset + entrySize > payload.Length)
            {
                return null;
            }
            var raw = MemoryMarshal.Read<DiscoveredEntryRaw>(payload.AsSpan(offset, entrySize));
            string mac;
            string timerName;
            string remoteName;
            unsafe
            {
                mac = TimerListItem.FormatMac(new ReadOnlySpan<byte>(raw.Mac, 6));
                timerName = Encoding.ASCII.GetString(new ReadOnlySpan<byte>(raw.TimerName, 10)).Trim('\0', ' ');
                remoteName = Encoding.ASCII.GetString(new ReadOnlySpan<byte>(raw.RemoteName, 10)).Trim('\0', ' ');
            }
            entries.Add(new DiscoveryEntry(raw.DiscoveryIndex, raw.PairedIndex, raw.Channel, raw.Rssi, mac, timerName, remoteName));
            offset += entrySize;
        }
        return new DiscoveryBatch(header.TotalCount, header.BatchStart, header.BatchCount, entries);
    }

    private void HandleDiscoveredDevices(DebugProtocol.Packet packet)
    {
        var batch = ParseDiscovery(packet);
        if (!batch.HasValue)
        {
            AppendLog("Discovery payload malformed.");
            return;
        }
        bool isFirst = batch.Value.BatchStart == 0;
        bool isFinal = batch.Value.BatchStart + batch.Value.BatchCount >= batch.Value.TotalCount || batch.Value.BatchCount == 0;
        ApplyDiscoveryBatch(batch.Value, isFirst, isFinal);
    }

    private void ApplyDiscoveryBatch(DiscoveryBatch batch, bool isFirstBatch, bool isFinalBatch)
    {
        if (isFirstBatch)
        {
            _discoveredMap.Clear();
            _discovered.Clear();
            DiscoveredList.SelectedItem = null;
            RenameInput.Text = string.Empty;
        }
        foreach (var entry in batch.Entries)
        {
            if (!_discoveredMap.TryGetValue(entry.Mac, out var item))
            {
                item = new DiscoveredListItem(entry.Mac);
                _discoveredMap[entry.Mac] = item;
            }
            item.Apply(entry.DiscoveryIndex, entry.PairedIndex, entry.Channel, entry.Rssi, entry.TimerName, entry.RemoteName);
        }
        if (isFinalBatch)
        {
            RebuildDiscoveryList();
        }
    }

    private void RebuildDiscoveryList()
    {
        var ordered = _discoveredMap.Values.OrderBy(item => item.DiscoveryIndex).ToList();
        _discovered.Clear();
        foreach (var item in ordered)
        {
            _discovered.Add(item);
        }
        UpdateDiscoverySelectionButtons();
        UpdateDiscoveryStatus();
    }

    private void UpdateDiscoveryControls()
    {
        bool connected = _client.IsConnected;
        bool running = _discoveryCts != null;
        StartDiscoveryButton.IsEnabled = connected && !running;
        StopDiscoveryButton.IsEnabled = connected && running;
        RefreshDiscoveryButton.IsEnabled = connected;
        UpdateDiscoverySelectionButtons();
        UpdateDiscoveryStatus();
    }

    private void UpdateDiscoverySelectionButtons()
    {
        bool connected = _client.IsConnected;
        if (DiscoveredList.SelectedItem is DiscoveredListItem item)
        {
            PairButton.IsEnabled = connected && !item.IsPaired;
            UnpairButton.IsEnabled = connected && item.IsPaired;
            RenameButton.IsEnabled = connected && item.IsPaired;
            RenameInput.IsEnabled = connected && item.IsPaired;
        }
        else
        {
            PairButton.IsEnabled = false;
            UnpairButton.IsEnabled = false;
            RenameButton.IsEnabled = false;
            RenameInput.IsEnabled = false;
        }
    }

    private void UpdateDiscoveryStatus()
    {
        string status;
        if (!_client.IsConnected)
        {
            status = "Discovery unavailable (not connected).";
        }
        else if (_discoveryCts != null)
        {
            status = "Discovery running.";
        }
        else
        {
            status = "Discovery idle.";
        }
        DiscoveryStatusText.Text = $"{status} {_discovered.Count} device(s).";
    }

    private async Task RunScannerLoopAsync(byte originalChannel, List<ScannerSession> sessions, CancellationToken token)
    {
        int cycle = 0;
        try
        {
            while (!token.IsCancellationRequested)
            {
                cycle++;
                Dispatcher.Invoke(() =>
                {
                    ScanStatusText.Text = $"Running (cycle {cycle})";
                });
                foreach (var session in sessions)
                {
                    if (token.IsCancellationRequested) break;
                    await RunScanForDeviceAsync(session, token).ConfigureAwait(false);
                }
            }
        }
        catch (OperationCanceledException)
        {
            // expected when cancelled
        }
        catch (Exception ex)
        {
            AppendLog($"Scan error: {ex.Message}");
        }
        finally
        {
            try
            {
                await RestoreChannelAsync(originalChannel).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                AppendLog($"Failed to restore channel: {ex.Message}");
            }
            Dispatcher.Invoke(() =>
            {
                ScanStatusText.Text = token.IsCancellationRequested ? "Stopped" : "Idle";
                CycleStatusText.Text = string.Empty;
                StartScanButton.IsEnabled = true;
                StopScanButton.IsEnabled = false;
            });
            _scanCts = null;
        }
    }

    private async Task RunScanForDeviceAsync(ScannerSession session, CancellationToken token)
    {
        try
        {
            var selectPayload = new[] { session.Timer.Index };
            var selectResponse = await _client.SendAsync(DebugProtocol.Command.SelectDevice, selectPayload, token).ConfigureAwait(false);
            ProcessResponse(selectResponse);
            Dispatcher.Invoke(() => SetActiveTimer(session.Timer.Index));
            Dispatcher.Invoke(() =>
            {
                CycleStatusText.Text = $"Device {session.Timer.DisplayName} (idx {session.Timer.Index})";
            });
            for (byte channel = 1; channel <= 13 && !token.IsCancellationRequested; channel++)
            {
                byte[] channelPayload = { channel, 1 };
                await _client.SendAsync(DebugProtocol.Command.ForceChannel, channelPayload, token).ConfigureAwait(false);
                bool settled = await WaitForChannelAsync(channel, session, token).ConfigureAwait(false);
                if (!settled)
                {
                    AppendLog($"Channel {channel} did not settle before sampling; continuing.");
                }
                int samplesRecorded = 0;
                int attempts = 0;
                int idx = channel - 1;
                while (samplesRecorded < session.SamplesPerChannel && !token.IsCancellationRequested)
                {
                    attempts++;
                    var remotePacket = await _client.SendAsync(DebugProtocol.Command.GetRemoteStats, Array.Empty<byte>(), token).ConfigureAwait(false);
                    var timerPacket = await _client.SendAsync(DebugProtocol.Command.GetTimerStats, Array.Empty<byte>(), token).ConfigureAwait(false);
                    var remotePayload = ExtractStruct<DebugProtocol.RemoteStatsPayload>(remotePacket);
                    var timerPayload = ExtractStruct<DebugProtocol.TimerStatsPayload>(timerPacket);
                    bool remoteStatusOk = remotePacket.Status == DebugProtocol.Status.Ok;
                    bool timerStatusOk = timerPacket.Status == DebugProtocol.Status.Ok;
                    bool remotePayloadValid = remotePayload.HasValue;
                    bool timerPayloadValid = timerPayload.HasValue;

                    if (remoteStatusOk && timerStatusOk && remotePayloadValid && timerPayloadValid)
                    {
                        var remoteValue = remotePayload.GetValueOrDefault();
                        var timerValue = timerPayload.GetValueOrDefault();
                        AppendSample(channel, remoteValue, timerValue);
                        samplesRecorded++;
                        if (idx >= 0 && idx < 13)
                        {
                            _remoteSampleFaultLogged[idx] = false;
                            _timerSampleFaultLogged[idx] = false;
                        }
                        int displaySample = samplesRecorded;
                        Dispatcher.Invoke(() =>
                        {
                            CycleStatusText.Text = $"Device {session.Timer.DisplayName} ch {channel} sample {displaySample}/{session.SamplesPerChannel}";
                        });
                    }
                    else
                    {
                        if (idx >= 0 && idx < 13)
                        {
                            if ((!remoteStatusOk || !remotePayloadValid) && !_remoteSampleFaultLogged[idx])
                            {
                                AppendLog($"Remote stats unavailable on channel {channel}: status {DebugProtocol.DescribeStatus(remotePacket.Status)} (len={remotePacket.DataLength})");
                                _remoteSampleFaultLogged[idx] = true;
                            }
                            if ((!timerStatusOk || !timerPayloadValid) && !_timerSampleFaultLogged[idx])
                            {
                                AppendLog($"Timer stats unavailable on channel {channel}: status {DebugProtocol.DescribeStatus(timerPacket.Status)} (len={timerPacket.DataLength})");
                                _timerSampleFaultLogged[idx] = true;
                            }
                        }
                        int retryCount = attempts - samplesRecorded;
                        Dispatcher.Invoke(() =>
                        {
                            CycleStatusText.Text = $"Device {session.Timer.DisplayName} ch {channel} retry {retryCount}";
                        });
                        if (attempts >= session.SamplesPerChannel * 3)
                        {
                            AppendLog($"Giving up on channel {channel} after {attempts} attempts without valid samples.");
                            break;
                        }
                    }

                    if (samplesRecorded < session.SamplesPerChannel && !token.IsCancellationRequested)
                    {
                        await Task.Delay(session.SampleDelayMs, token).ConfigureAwait(false);
                    }
                }
            }
        }
        catch (OperationCanceledException)
        {
            // cancellation expected
        }
        catch (Exception ex)
        {
            AppendLog($"Scan error for {session.Timer.DisplayName}: {ex.Message}");
        }
    }

    private async Task<bool> WaitForChannelAsync(byte channel, ScannerSession session, CancellationToken token)
    {
        int maxAttempts = Math.Max(1, session.SettleDelayMs / 40);
        maxAttempts = Math.Clamp(maxAttempts, 3, 15);
        for (int attempt = 0; attempt < maxAttempts && !token.IsCancellationRequested; attempt++)
        {
            var remotePacket = await _client.SendAsync(DebugProtocol.Command.GetRemoteStats, Array.Empty<byte>(), token).ConfigureAwait(false);
            var remotePayload = ExtractStruct<DebugProtocol.RemoteStatsPayload>(remotePacket);
            bool remoteReady = remotePacket.Status == DebugProtocol.Status.Ok && remotePayload.HasValue && remotePayload.Value.RemoteLink.Channel == channel;

            bool timerReady = false;
            if (remoteReady)
            {
                var timerPacket = await _client.SendAsync(DebugProtocol.Command.GetTimerStats, Array.Empty<byte>(), token).ConfigureAwait(false);
                var timerPayload = ExtractStruct<DebugProtocol.TimerStatsPayload>(timerPacket);
                timerReady = timerPacket.Status == DebugProtocol.Status.Ok && timerPayload.HasValue && timerPayload.Value.Link.Channel == channel;
                if (!timerReady && !_timerSampleFaultLogged[channel - 1])
                {
                    AppendLog($"Timer not yet on channel {channel}: status {DebugProtocol.DescribeStatus(timerPacket.Status)}");
                    _timerSampleFaultLogged[channel - 1] = true;
                }
            }
            else if (!_remoteSampleFaultLogged[channel - 1])
            {
                AppendLog($"Remote still switching to channel {channel}: status {DebugProtocol.DescribeStatus(remotePacket.Status)}");
                _remoteSampleFaultLogged[channel - 1] = true;
            }

            if (remoteReady && timerReady)
            {
                _remoteSampleFaultLogged[channel - 1] = false;
                _timerSampleFaultLogged[channel - 1] = false;
                return true;
            }

            int delayMs = Math.Max(20, session.SettleDelayMs / 2);
            await Task.Delay(delayMs, token).ConfigureAwait(false);
        }
        return false;
    }

    private async Task RestoreChannelAsync(byte channel)
    {
        if (!_client.IsConnected) return;
        byte[] payload = { channel, (byte)1 };
        await _client.SendAsync(DebugProtocol.Command.ForceChannel, payload, CancellationToken.None).ConfigureAwait(false);
    }

    private void SetActiveTimer(byte index)
    {
        foreach (var item in _timers)
        {
            item.SetActive(item.Index == index);
        }
        UpdateSelectionSummary();
    }

    private void AppendSample(byte channel, DebugProtocol.RemoteStatsPayload remote, DebugProtocol.TimerStatsPayload timer)
    {
        var remoteRssi = BestRssi(remote.RemoteLink.RssiPeer, remote.RemoteLink.RssiLocal);
        var timerRssi = BestRssi(timer.Link.RssiPeer, timer.Link.RssiLocal);
        Dispatcher.Invoke(() =>
        {
            _remotePlot.AddSample(channel, remoteRssi, false);
            _timerPlot.AddSample(channel, timerRssi, true);
            UpdateHeatmapAxes();
            if (channel < _minSampledChannel) _minSampledChannel = channel;
            if (channel > _maxSampledChannel) _maxSampledChannel = channel;
            int idx = channel - 1;
            if (idx >= 0 && idx < 13)
            {
                _remoteSampleCounts[idx]++;
                _timerSampleCounts[idx]++;
                _remoteLastRssi[idx] = remoteRssi;
                _timerLastRssi[idx] = timerRssi;
            }
            _remotePlot.SetDebugInfo(_remoteSampleCounts, _remoteLastRssi);
            _timerPlot.SetDebugInfo(_timerSampleCounts, _timerLastRssi);
        });
    }

    private static sbyte BestRssi(sbyte primary, sbyte fallback) => primary != 0 ? primary : fallback;

    private void TimerCheckBox_OnChanged(object sender, RoutedEventArgs e)
    {
        UpdateSelectionSummary();
    }

    private void UpdateSelectionSummary()
    {
        int total = _timers.Count;
        int selected = _timers.Count(t => t.IsSelected);
        SelectionSummaryText.Text = total == 0 ? "No timers loaded." : $"Selected {selected} of {total}.";
        UpdateTimerControlState();
    }

    private void UpdateHeatmapAxes()
    {
        UpdateAxisLabels(_remotePlot, RemoteAxisMaxText, RemoteAxisMidText, RemoteAxisMinText);
        UpdateAxisLabels(_timerPlot, TimerAxisMaxText, TimerAxisMidText, TimerAxisMinText);
    }

    private static void UpdateAxisLabels(HeatBarPlot plot, TextBlock maxText, TextBlock midText, TextBlock minText)
    {
        var range = plot.GetRange();
        string maxLabel = $"{range.maxDbm.ToString(CultureInfo.InvariantCulture)} dBm";
        string minLabel = $"{range.minDbm.ToString(CultureInfo.InvariantCulture)} dBm";
        int midValue = (int)Math.Round((range.maxDbm + range.minDbm) / 2.0);
        string midLabel = $"{midValue.ToString(CultureInfo.InvariantCulture)} dBm";
        maxText.Text = maxLabel;
        midText.Text = midLabel;
        minText.Text = minLabel;
        double opacity = range.hasSamples ? 1.0 : 0.6;
        maxText.Opacity = opacity;
        midText.Opacity = opacity;
        minText.Opacity = opacity;
    }

    private void UpdateTimerControlState()
    {
        bool connected = _client.IsConnected;
        var selected = GetSelectedTimers();
        bool hasSelection = selected.Count > 0;

        ApplyTimerValuesButton.IsEnabled = connected && hasSelection;
        ActivateOutputButton.IsEnabled = connected && hasSelection;
        DeactivateOutputButton.IsEnabled = connected && hasSelection;

        if (selected.Count == 1)
        {
            var item = selected[0];
            SetTimerInputIfNotFocused(TimerTonInput, item.TonSeconds);
            SetTimerInputIfNotFocused(TimerToffInput, item.ToffSeconds);
        }
        else if (selected.Count == 0)
        {
            var active = _timers.FirstOrDefault(t => t.IsActive);
            if (active != null)
            {
                SetTimerInputIfNotFocused(TimerTonInput, active.TonSeconds);
                SetTimerInputIfNotFocused(TimerToffInput, active.ToffSeconds);
            }
        }
        else
        {
            if (!TimerTonInput.IsFocused)
            {
                TimerTonInput.Text = string.Empty;
            }
            if (!TimerToffInput.IsFocused)
            {
                TimerToffInput.Text = string.Empty;
            }
        }
    }

    private void SetTimerInputIfNotFocused(TextBox box, float value)
    {
        if (box.IsFocused)
        {
            return;
        }
        if (value > 0.0f)
        {
            box.Text = value.ToString("F1", CultureInfo.InvariantCulture);
        }
        else
        {
            box.Text = string.Empty;
        }
    }

    private List<TimerListItem> GetSelectedTimers() => _timers.Where(t => t.IsSelected).ToList();

    private List<TimerListItem> ResolveTimerTargets()
    {
        var targets = GetSelectedTimers();
        if (targets.Count == 0)
        {
            var active = _timers.FirstOrDefault(t => t.IsActive);
            if (active != null)
            {
                targets.Add(active);
            }
        }
        return targets;
    }

    private void SetTimerControlStatus(string message)
    {
        TimerControlStatusText.Text = message;
    }

    private bool TryGetTimerInputs(out float ton, out float toff)
    {
        ton = 0f;
        toff = 0f;
        if (!float.TryParse(TimerTonInput.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out ton))
        {
            return false;
        }
        if (!float.TryParse(TimerToffInput.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out toff))
        {
            return false;
        }
        if (ton < 0f || toff < 0f)
        {
            return false;
        }
        return true;
    }

    private async void ApplyTimerValuesButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (!_client.IsConnected)
        {
            SetTimerControlStatus("Connect before applying values.");
            return;
        }
        if (!TryGetTimerInputs(out float ton, out float toff))
        {
            SetTimerControlStatus("Enter valid Ton/Toff values (>= 0).");
            return;
        }

        var targets = ResolveTimerTargets();
        if (targets.Count == 0)
        {
            SetTimerControlStatus("Load timers and select at least one entry.");
            return;
        }

        SetTimerControlStatus("Applying timer values...");
        try
        {
            foreach (var timer in targets)
            {
                var payload = new byte[1 + sizeof(float) * 2];
                payload[0] = timer.Index;
                Buffer.BlockCopy(BitConverter.GetBytes(ton), 0, payload, 1, sizeof(float));
                Buffer.BlockCopy(BitConverter.GetBytes(toff), 0, payload, 1 + sizeof(float), sizeof(float));
                var response = await _client.SendAsync(DebugProtocol.Command.SetTimerValues, payload, _uiCts.Token);
                if (response.Status != DebugProtocol.Status.Ok)
                {
                    SetTimerControlStatus($"Failed on {timer.DisplayName}: {DebugProtocol.DescribeStatus(response.Status)}");
                    return;
                }
                timer.UpdateSnapshot(new DebugProtocol.TimerSnapshot
                {
                    TonSeconds = ton,
                    ToffSeconds = toff,
                    OutputOn = timer.OutputOn ? (byte)1 : (byte)0,
                    Channel = timer.Channel
                });
            }
            SetTimerControlStatus($"Applied Ton {ton:F1}s / Toff {toff:F1}s to {targets.Count} timer(s).");
            UpdateTimerControlState();
        }
        catch (OperationCanceledException)
        {
            SetTimerControlStatus("Apply canceled.");
        }
        catch (Exception ex)
        {
            SetTimerControlStatus($"Apply failed: {ex.Message}");
        }
    }

    private async void ActivateOutputButton_OnClick(object sender, RoutedEventArgs e)
    {
        await ApplyOutputOverrideAsync(true);
    }

    private async void DeactivateOutputButton_OnClick(object sender, RoutedEventArgs e)
    {
        await ApplyOutputOverrideAsync(false);
    }

    private async Task ApplyOutputOverrideAsync(bool desiredOn)
    {
        if (!_client.IsConnected)
        {
            SetTimerControlStatus("Connect before controlling output.");
            return;
        }

        var targets = ResolveTimerTargets();
        if (targets.Count == 0)
        {
            SetTimerControlStatus("Load timers and select at least one entry.");
            return;
        }

        SetTimerControlStatus(desiredOn ? "Activating output..." : "Deactivating output...");
        byte mode = desiredOn ? (byte)1 : (byte)0;
        try
        {
            foreach (var timer in targets)
            {
                var payload = new[] { timer.Index, mode };
                var response = await _client.SendAsync(DebugProtocol.Command.SetTimerOutput, payload, _uiCts.Token);
                if (response.Status != DebugProtocol.Status.Ok)
                {
                    SetTimerControlStatus($"Failed on {timer.DisplayName}: {DebugProtocol.DescribeStatus(response.Status)}");
                    return;
                }
                timer.UpdateSnapshot(new DebugProtocol.TimerSnapshot
                {
                    TonSeconds = timer.TonSeconds,
                    ToffSeconds = timer.ToffSeconds,
                    OutputOn = mode,
                    Channel = timer.Channel
                });
            }
            SetTimerControlStatus(desiredOn ? $"Activated {targets.Count} timer(s)." : $"Deactivated {targets.Count} timer(s).");
            UpdateTimerControlState();
        }
        catch (OperationCanceledException)
        {
            SetTimerControlStatus("Command canceled.");
        }
        catch (Exception ex)
        {
            SetTimerControlStatus((desiredOn ? "Activate" : "Deactivate") + $" failed: {ex.Message}");
        }
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    private struct DeviceInventoryHeader
    {
        public byte TotalCount;
        public byte BatchStart;
        public byte BatchCount;
        public byte ActiveIndex;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    private unsafe struct DeviceInventoryEntryRaw
    {
        public byte Index;
        public byte Channel;
        public byte Reserved0;
        public byte Reserved1;
        public fixed byte Mac[6];
        public fixed byte Name[10];
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    private struct DiscoveryHeader
    {
        public byte TotalCount;
        public byte BatchStart;
        public byte BatchCount;
        public byte Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    private unsafe struct DiscoveredEntryRaw
    {
        public byte DiscoveryIndex;
        public byte PairedIndex;
        public byte Channel;
        public sbyte Rssi;
        public fixed byte Mac[6];
        public fixed byte TimerName[10];
        public fixed byte RemoteName[10];
    }

    private readonly record struct InventoryEntry(byte Index, byte Channel, string Name, string Mac);

    private readonly record struct DiscoveryEntry(byte DiscoveryIndex, byte PairedIndex, byte Channel, sbyte Rssi, string Mac, string TimerName, string RemoteName);

    private readonly record struct InventoryBatch(byte TotalCount, byte BatchStart, byte BatchCount, byte ActiveIndex, List<InventoryEntry> Entries);

    private readonly record struct DiscoveryBatch(byte TotalCount, byte BatchStart, byte BatchCount, List<DiscoveryEntry> Entries);
}

internal sealed class TimerListItem : INotifyPropertyChanged
{
    private string _displayName;
    private string _macString = string.Empty;
    private bool _isActive;
    private bool _isSelected;
    private byte _channel;
    private float _tonSeconds;
    private float _toffSeconds;
    private bool _outputOn;

    public TimerListItem(byte index)
    {
        Index = index;
        _displayName = $"Timer {index}";
    }

    public byte Index { get; }

    public string DisplayName
    {
        get => _displayName;
        private set => SetField(ref _displayName, value);
    }

    public string MacString
    {
        get => _macString;
        private set => SetField(ref _macString, value);
    }

    public bool IsActive
    {
        get => _isActive;
        private set
        {
            if (SetField(ref _isActive, value))
            {
                OnPropertyChanged(nameof(ActiveMarker));
            }
        }
    }

    public bool IsSelected
    {
        get => _isSelected;
        set => SetField(ref _isSelected, value);
    }

    public byte Channel
    {
        get => _channel;
        private set
        {
            if (SetField(ref _channel, value))
            {
                OnPropertyChanged(nameof(ChannelText));
                OnPropertyChanged(nameof(SummaryText));
            }
        }
    }

    public string ActiveMarker => IsActive ? "(active)" : string.Empty;

    public string ChannelText => Channel >= 1 && Channel <= 13 ? $"Channel {Channel}" : "Channel ?";

    public float TonSeconds
    {
        get => _tonSeconds;
        private set
        {
            if (SetField(ref _tonSeconds, value))
            {
                OnPropertyChanged(nameof(SummaryText));
            }
        }
    }

    public float ToffSeconds
    {
        get => _toffSeconds;
        private set
        {
            if (SetField(ref _toffSeconds, value))
            {
                OnPropertyChanged(nameof(SummaryText));
            }
        }
    }

    public bool OutputOn
    {
        get => _outputOn;
        private set
        {
            if (SetField(ref _outputOn, value))
            {
                OnPropertyChanged(nameof(SummaryText));
            }
        }
    }

    public string SummaryText
    {
        get
        {
            string ton = TonSeconds > 0.01f ? TonSeconds.ToString("F1", CultureInfo.InvariantCulture) : "?";
            string toff = ToffSeconds > 0.01f ? ToffSeconds.ToString("F1", CultureInfo.InvariantCulture) : "?";
            string state = OutputOn ? "ON" : "OFF";
            return $"Ton {ton}s / Toff {toff}s | Output {state}";
        }
    }

    public void Apply(byte channel, string name, string mac, bool isActive)
    {
        Channel = channel;
        string trimmed = string.IsNullOrWhiteSpace(name) ? $"Timer {Index}" : name.Trim();
        DisplayName = trimmed;
        MacString = mac;
        IsActive = isActive;
        if (IsActive && !IsSelected)
        {
            IsSelected = true;
        }
        OnPropertyChanged(nameof(SummaryText));
    }

    public void SetActive(bool value)
    {
        IsActive = value;
        if (IsActive && !IsSelected)
        {
            IsSelected = true;
        }
        OnPropertyChanged(nameof(SummaryText));
    }

    public void UpdateSnapshot(DebugProtocol.TimerSnapshot snapshot)
    {
        TonSeconds = snapshot.TonSeconds;
        ToffSeconds = snapshot.ToffSeconds;
        OutputOn = snapshot.OutputOn != 0;
        if (snapshot.Channel >= 1 && snapshot.Channel <= 13)
        {
            Channel = snapshot.Channel;
        }
    }

    public static string FormatMac(ReadOnlySpan<byte> mac)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < mac.Length; i++)
        {
            if (i > 0) sb.Append(':');
            sb.Append(mac[i].ToString("X2"));
        }
        return sb.ToString();
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }

    private void OnPropertyChanged(string? propertyName)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}

internal sealed class DiscoveredListItem : INotifyPropertyChanged
{
    private byte _discoveryIndex;
    private byte _pairedIndex = 0xFF;
    private byte _channel;
    private sbyte _rssi;
    private string _timerName = string.Empty;
    private string _remoteName = string.Empty;

    public DiscoveredListItem(string macString)
    {
        MacString = macString;
    }

    public string MacString { get; }

    public byte DiscoveryIndex
    {
        get => _discoveryIndex;
        private set => SetField(ref _discoveryIndex, value);
    }

    public byte PairedIndex
    {
        get => _pairedIndex;
        private set
        {
            if (SetField(ref _pairedIndex, value))
            {
                OnPropertyChanged(nameof(IsPaired));
                OnPropertyChanged(nameof(PairingText));
            }
        }
    }

    public bool IsPaired => PairedIndex != 0xFF;

    public byte Channel
    {
        get => _channel;
        private set
        {
            if (SetField(ref _channel, value))
            {
                OnPropertyChanged(nameof(MetaText));
            }
        }
    }

    public sbyte Rssi
    {
        get => _rssi;
        private set
        {
            if (SetField(ref _rssi, value))
            {
                OnPropertyChanged(nameof(MetaText));
            }
        }
    }

    public string TimerName
    {
        get => _timerName;
        private set
        {
            if (SetField(ref _timerName, value))
            {
                OnPropertyChanged(nameof(Title));
                OnPropertyChanged(nameof(PairingText));
            }
        }
    }

    public string RemoteName
    {
        get => _remoteName;
        private set
        {
            if (SetField(ref _remoteName, value))
            {
                OnPropertyChanged(nameof(PairingText));
            }
        }
    }

    public string Title => string.IsNullOrWhiteSpace(TimerName) ? "(Unnamed timer)" : TimerName;

    public string PairingText
    {
        get
        {
            if (!IsPaired)
            {
                return "Unpaired";
            }
            string remote = string.IsNullOrWhiteSpace(RemoteName) ? "(remote alias empty)" : RemoteName;
            if (string.IsNullOrWhiteSpace(TimerName) || string.Equals(TimerName, RemoteName, StringComparison.Ordinal))
            {
                return $"Paired (remote alias: {remote})";
            }
            return $"Paired (remote alias: {remote}, timer reports: {TimerName})";
        }
    }

    public string MetaText
    {
        get
        {
            string channel = Channel >= 1 && Channel <= 13 ? Channel.ToString() : "?";
            string rssi = Rssi <= 0 ? $"{Rssi} dBm" : "? dBm";
            return $"MAC {MacString} | Channel {channel} | RSSI {rssi}";
        }
    }

    public void Apply(byte discoveryIndex, byte pairedIndex, byte channel, sbyte rssi, string timerName, string remoteName)
    {
        DiscoveryIndex = discoveryIndex;
        PairedIndex = pairedIndex;
        Channel = channel;
        Rssi = rssi;
        TimerName = timerName;
        RemoteName = remoteName;
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }

    private void OnPropertyChanged(string? propertyName)
    {
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}

internal sealed class ScannerSession
{
    public ScannerSession(TimerListItem timer, int samplesPerChannel, int settleDelayMs, int sampleDelayMs)
    {
        Timer = timer;
        SamplesPerChannel = samplesPerChannel;
        SettleDelayMs = settleDelayMs;
        SampleDelayMs = sampleDelayMs;
    }

    public TimerListItem Timer { get; }
    public int SamplesPerChannel { get; }
    public int SettleDelayMs { get; }
    public int SampleDelayMs { get; }
}

internal sealed class HeatBarPlot
{
    private const byte ChannelCount = 13;
    private const int DefaultMinDbm = -100;
    private const int DefaultMaxDbm = -30;
    private const int BackgroundColor = unchecked((int)0xFF101018);
    private const int ColumnBaseColor = unchecked((int)0xFF1C2534);
    private const int ColumnSeparatorColor = unchecked((int)0xFF0C111B);
    private const int BarBackgroundColor = unchecked((int)0xFF232D40);
    private readonly int _width;
    private readonly int _height;
    private readonly int _columnWidth;
    private readonly WriteableBitmap _bitmap;
    private readonly Dictionary<int, int>[] _histograms;
    private readonly int[] _debugCounts = new int[ChannelCount];
    private readonly sbyte[] _debugLastRssi = new sbyte[ChannelCount];
    private bool _hasSamples;
    private int _globalMinDbm = DefaultMinDbm;
    private int _globalMaxDbm = DefaultMaxDbm;
    private int _globalMaxCount = 1;

    public HeatBarPlot(int width, int height)
    {
        _width = Math.Max(width, ChannelCount);
        _height = Math.Max(height, 1);
        _columnWidth = Math.Max(1, _width / ChannelCount);
        _bitmap = new WriteableBitmap(_width, _height, 96, 96, System.Windows.Media.PixelFormats.Bgra32, null);
        _histograms = new Dictionary<int, int>[ChannelCount];
        for (int i = 0; i < ChannelCount; i++)
        {
            _histograms[i] = new Dictionary<int, int>();
        }
        RenderAll();
    }

    public WriteableBitmap Bitmap => _bitmap;

    public void Clear()
    {
        for (int i = 0; i < ChannelCount; i++)
        {
            _histograms[i].Clear();
        }
        _hasSamples = false;
        _globalMinDbm = DefaultMinDbm;
        _globalMaxDbm = DefaultMaxDbm;
        _globalMaxCount = 1;
        RenderAll();
    }

    public void AddSample(byte channel, sbyte rssi, bool _)
    {
        if (channel < 1 || channel > ChannelCount)
        {
            channel = 1;
        }
        int dbm = Math.Clamp((int)rssi, -120, 0);
        int index = channel - 1;
        var hist = _histograms[index];
        hist.TryGetValue(dbm, out int current);
        hist[dbm] = current + 1;

        bool rangeChanged = false;
        bool scaleChanged = false;

        if (!_hasSamples)
        {
            _hasSamples = true;
            _globalMinDbm = dbm;
            _globalMaxDbm = dbm;
            rangeChanged = true;
        }
        if (dbm < _globalMinDbm)
        {
            _globalMinDbm = dbm;
            rangeChanged = true;
        }
        if (dbm > _globalMaxDbm)
        {
            _globalMaxDbm = dbm;
            rangeChanged = true;
        }
        if (hist[dbm] > _globalMaxCount)
        {
            _globalMaxCount = hist[dbm];
            scaleChanged = true;
        }

        _debugCounts[index]++;
        _debugLastRssi[index] = rssi;

        if (rangeChanged || scaleChanged)
        {
            RenderAll();
        }
        else
        {
            RenderChannel(index);
        }
    }

    public void SetDebugInfo(int[] counts, sbyte[] lastRssi)
    {
        for (int i = 0; i < ChannelCount; i++)
        {
            _debugCounts[i] = (counts != null && i < counts.Length) ? counts[i] : 0;
            _debugLastRssi[i] = (lastRssi != null && i < lastRssi.Length) ? lastRssi[i] : (sbyte)0;
        }
        RenderAll();
    }

    public (bool hasSamples, int minDbm, int maxDbm) GetRange()
    {
        return _hasSamples ? (true, _globalMinDbm, _globalMaxDbm) : (false, DefaultMinDbm, DefaultMaxDbm);
    }

    private void RenderAll()
    {
        RenderInternal(-1);
    }

    private void RenderChannel(int index)
    {
        RenderInternal(index);
    }

    private unsafe void RenderInternal(int channelIndex)
    {
        int minDbm = _hasSamples ? _globalMinDbm : DefaultMinDbm;
        int maxDbm = _hasSamples ? _globalMaxDbm : DefaultMaxDbm;
        if (minDbm > maxDbm)
        {
            (minDbm, maxDbm) = (maxDbm, minDbm);
        }
        int levels = Math.Max(1, maxDbm - minDbm + 1);
        double pixelsPerLevel = (double)_height / levels;

        _bitmap.Lock();
        try
        {
            Span<int> pixels = new(_bitmap.BackBuffer.ToPointer(), _width * _height);
            if (channelIndex < 0)
            {
                pixels.Fill(BackgroundColor);
                for (int ch = 0; ch < ChannelCount; ch++)
                {
                    PaintColumnBaseline(pixels, ch);
                }
            }
            else
            {
                var (start, width) = GetColumnBounds(channelIndex);
                ClearColumn(pixels, start, width);
                PaintColumnBaseline(pixels, channelIndex);
            }

            if (_hasSamples)
            {
                if (channelIndex < 0)
                {
                    for (int ch = 0; ch < ChannelCount; ch++)
                    {
                        RenderColumn(pixels, ch, minDbm, maxDbm, pixelsPerLevel);
                    }
                }
                else
                {
                    RenderColumn(pixels, channelIndex, minDbm, maxDbm, pixelsPerLevel);
                }
            }

            if (channelIndex < 0)
            {
                _bitmap.AddDirtyRect(new Int32Rect(0, 0, _width, _height));
            }
            else
            {
                var (start, width) = GetColumnBounds(channelIndex);
                _bitmap.AddDirtyRect(new Int32Rect(start, 0, width, _height));
            }
        }
        finally
        {
            _bitmap.Unlock();
        }
    }

    private (int start, int width) GetColumnBounds(int index)
    {
        int start = index * _columnWidth;
        int end = (index == ChannelCount - 1) ? _width : Math.Min(_width, start + _columnWidth);
        int width = Math.Max(1, end - start);
        return (start, width);
    }

    private void ClearColumn(Span<int> pixels, int start, int width)
    {
        for (int y = 0; y < _height; y++)
        {
            int rowOffset = y * _width + start;
            for (int x = 0; x < width; x++)
            {
                pixels[rowOffset + x] = BackgroundColor;
            }
        }
    }

    private void PaintColumnBaseline(Span<int> pixels, int channelIndex)
    {
        var (start, width) = GetColumnBounds(channelIndex);
        for (int y = 0; y < _height; y++)
        {
            int rowOffset = y * _width + start;
            for (int x = 0; x < width; x++)
            {
                pixels[rowOffset + x] = ColumnBaseColor;
            }
            pixels[rowOffset + width - 1] = ColumnSeparatorColor;
        }

        // draw simple tick marks for debug info at bottom 3 rows (count) and top pixel last RSSI indicator
        int count = _debugCounts[channelIndex];
        if (count > 0)
        {
            byte intensity = (byte)Math.Clamp(40 + Math.Min(count, 50) * 4, 40, 220);
            int debugColor = unchecked((int)(0xFF000000u | ((uint)intensity << 16) | ((uint)intensity << 8) | intensity));
            for (int y = _height - 3; y < _height; y++)
            {
                int rowOffset = y * _width + start;
                for (int x = 0; x < width - 1; x++)
                {
                    pixels[rowOffset + x] = debugColor;
                }
            }
        }
        sbyte last = _debugLastRssi[channelIndex];
        if (count > 0)
        {
            // draw a single pixel at the corresponding level so it is obvious what the last sample was
            int level = Math.Clamp((int)last, -120, 0);
            double normalized = (level - _globalMinDbm) / Math.Max(1, _globalMaxDbm - _globalMinDbm);
            int y = _height - 1 - (int)Math.Round(normalized * (_height - 1));
            y = Math.Clamp(y, 0, _height - 1);
            int offset = y * _width + start + width / 2;
            pixels[offset] = unchecked((int)0xFFFFFFFF);
        }
    }

    private void RenderColumn(Span<int> pixels, int channelIndex, int minDbm, int maxDbm, double pixelsPerLevel)
    {
        var hist = _histograms[channelIndex];
        if (hist.Count == 0)
        {
            return;
        }

        var (start, width) = GetColumnBounds(channelIndex);
        int channelMin = Math.Clamp(hist.Keys.Min(), minDbm, maxDbm);
        int channelMax = Math.Clamp(hist.Keys.Max(), minDbm, maxDbm);
        var (barTop, _) = RowBoundsFor(channelMax, minDbm, maxDbm, pixelsPerLevel);
        var (_, barBottom) = RowBoundsFor(channelMin, minDbm, maxDbm, pixelsPerLevel);

        var sortedLevels = hist.Keys.OrderBy(level => level).ToList();

        for (int y = barTop; y < barBottom; y++)
        {
            int rowOffset = y * _width + start;
            for (int x = 0; x < width; x++)
            {
                pixels[rowOffset + x] = BarBackgroundColor;
            }
        }

        if (sortedLevels.Count == 0)
        {
            return;
        }

        for (int y = barTop; y < barBottom; y++)
        {
            double level = maxDbm - ((y + 0.5) / pixelsPerLevel);
            level = Math.Clamp(level, channelMin, channelMax);
            uint color = SampleColorForLevel(hist, sortedLevels, level, _globalMaxCount);
            int packed = unchecked((int)color);
            int rowOffset = y * _width + start;
            for (int x = 0; x < width; x++)
            {
                pixels[rowOffset + x] = packed;
            }
        }
    }

    private (int top, int bottom) RowBoundsFor(int dbm, int minDbm, int maxDbm, double pixelsPerLevel)
    {
        dbm = Math.Clamp(dbm, minDbm, maxDbm);
        double top = (maxDbm - dbm) * pixelsPerLevel;
        double bottom = top + pixelsPerLevel;
        int topRow = (int)Math.Floor(top);
        int bottomRow = (int)Math.Ceiling(bottom);
        if (bottomRow <= topRow)
        {
            bottomRow = topRow + 1;
        }
        topRow = Math.Clamp(topRow, 0, _height - 1);
        bottomRow = Math.Clamp(bottomRow, topRow + 1, _height);
        return (topRow, bottomRow);
    }

    private static uint HeatColor(int count, int maxCount)
    {
        if (maxCount <= 0)
        {
            maxCount = 1;
        }
        double normalized = Math.Clamp((double)count / maxCount, 0.0, 1.0);
        return InterpolateHeat(normalized);
    }

    private static uint InterpolateHeat(double t)
    {
        t = Math.Clamp(t, 0.0, 1.0);
        double r;
        double g;
        double b;
        if (t <= 0.5)
        {
            double local = t / 0.5;
            r = 1.0;
            g = (60.0 + local * (225.0 - 60.0)) / 255.0;
            b = 32.0 / 255.0;
        }
        else
        {
            double local = (t - 0.5) / 0.5;
            r = (255.0 - local * 255.0) / 255.0;
            g = 1.0;
            b = (32.0 + local * (160.0 - 32.0)) / 255.0;
        }
        uint ur = (uint)Math.Clamp((int)Math.Round(r * 255.0), 0, 255);
        uint ug = (uint)Math.Clamp((int)Math.Round(g * 255.0), 0, 255);
        uint ub = (uint)Math.Clamp((int)Math.Round(b * 255.0), 0, 255);
        return 0xFF000000u | (ur << 16) | (ug << 8) | ub;
    }

    private static uint SampleColorForLevel(Dictionary<int, int> hist, List<int> sortedLevels, double targetLevel, int maxCount)
    {
        if (sortedLevels.Count == 0)
        {
            return unchecked((uint)BarBackgroundColor);
        }

        double epsilon = 1e-3;

        for (int i = 0; i < sortedLevels.Count; i++)
        {
            int level = sortedLevels[i];
            if (Math.Abs(targetLevel - level) <= epsilon)
            {
                return HeatColor(hist[level], maxCount);
            }
        }

        if (targetLevel <= sortedLevels[0])
        {
            return HeatColor(hist[sortedLevels[0]], maxCount);
        }
        if (targetLevel >= sortedLevels[^1])
        {
            return HeatColor(hist[sortedLevels[^1]], maxCount);
        }

        int upperIndex = 0;
        while (upperIndex < sortedLevels.Count && sortedLevels[upperIndex] < targetLevel)
        {
            upperIndex++;
        }

        int upperLevel = sortedLevels[upperIndex];
        int lowerLevel = sortedLevels[upperIndex - 1];

        double span = upperLevel - lowerLevel;
        if (span <= epsilon)
        {
            return HeatColor(hist[upperLevel], maxCount);
        }

        double weight = (targetLevel - lowerLevel) / span;
        uint lowerColor = HeatColor(hist[lowerLevel], maxCount);
        uint upperColor = HeatColor(hist[upperLevel], maxCount);
        return LerpColor(lowerColor, upperColor, weight);
    }

    private static uint LerpColor(uint c0, uint c1, double t)
    {
        t = Math.Clamp(t, 0.0, 1.0);
        double r0 = ((c0 >> 16) & 0xFF) / 255.0;
        double g0 = ((c0 >> 8) & 0xFF) / 255.0;
        double b0 = (c0 & 0xFF) / 255.0;
        double r1 = ((c1 >> 16) & 0xFF) / 255.0;
        double g1 = ((c1 >> 8) & 0xFF) / 255.0;
        double b1 = (c1 & 0xFF) / 255.0;

        double r = r0 + (r1 - r0) * t;
        double g = g0 + (g1 - g0) * t;
        double b = b0 + (b1 - b0) * t;

        uint ur = (uint)Math.Clamp((int)Math.Round(r * 255.0), 0, 255);
        uint ug = (uint)Math.Clamp((int)Math.Round(g * 255.0), 0, 255);
        uint ub = (uint)Math.Clamp((int)Math.Round(b * 255.0), 0, 255);
        return 0xFF000000u | (ur << 16) | (ug << 8) | ub;
    }
}
