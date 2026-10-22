# Finds the program that has a serial port open ("COM5 is in use by another program" / "Access to the path 'COM5' is denied").
#
#   powershell -ExecutionPolicy Bypass -File toolsind-port-holder.ps1                 # looks for USB serial devices (USBSER)
#   powershell -ExecutionPolicy Bypass -File toolsind-port-holder.ps1 -Match COM5     # or any text in the device name
#
# Run it from an ELEVATED PowerShell (Run as administrator) to see services and other users' programs too; without that it
# only sees your own programs. It only reads the system's handle table: nothing is closed or changed.
param([string]$Match = "USBSER")

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class HandleFinder
{
    [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int cls, IntPtr buf, int len, out int ret);
    [DllImport("ntdll.dll")] static extern int NtQueryObject(IntPtr h, int cls, IntPtr buf, int len, out int ret);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool DuplicateHandle(IntPtr src, IntPtr srcH, IntPtr dst, out IntPtr dstH, uint access, bool inherit, uint opts);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();

    static string NameOf(IntPtr h)
    {
        string result = null;
        var t = new Thread(() =>
        {
            IntPtr buf = Marshal.AllocHGlobal(2048);
            try
            {
                int ret;
                if (NtQueryObject(h, 1, buf, 2048, out ret) == 0)
                {
                    int len = Marshal.ReadInt16(buf);
                    IntPtr str = Marshal.ReadIntPtr(buf, IntPtr.Size);
                    if (len > 0 && str != IntPtr.Zero) result = Marshal.PtrToStringUni(str, len / 2);
                }
            }
            finally { Marshal.FreeHGlobal(buf); }
        }) { IsBackground = true };
        t.Start();
        return t.Join(400) ? result : null;           // a hung query (named pipe) is abandoned
    }

    public static List<string> Find(string match)
    {
        var found = new List<string>();
        // type index of "File" objects: open a file ourselves and look at its handle entry
        int size = 1 << 20, ret;
        IntPtr buf = Marshal.AllocHGlobal(size);
        while (NtQuerySystemInformation(64, buf, size, out ret) == unchecked((int)0xC0000004))
        {
            Marshal.FreeHGlobal(buf);
            size = ret + (1 << 16);
            buf = Marshal.AllocHGlobal(size);
        }
        long count = Marshal.ReadInt64(buf);
        int entry = 40;                                // SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX on x64
        var self = Process.GetCurrentProcess().Id;

        ushort fileType = 0;
        using (var fs = File.OpenRead(Environment.GetEnvironmentVariable("ComSpec")))
        {
            long myHandle = fs.SafeFileHandle.DangerousGetHandle().ToInt64();
            for (long i = 0; i < count; i++)
            {
                IntPtr p = buf + 16 + (int)(i * entry);
                long pid = Marshal.ReadInt64(p, 8);
                long hv = Marshal.ReadInt64(p, 16);
                if (pid == self && hv == myHandle) { fileType = (ushort)Marshal.ReadInt16(p, 28); break; }
            }
        }

        var procs = new Dictionary<int, IntPtr>();
        for (long i = 0; i < count; i++)
        {
            IntPtr p = buf + 16 + (int)(i * entry);
            ushort type = (ushort)Marshal.ReadInt16(p, 28);
            if (type != fileType) continue;
            int pid = (int)Marshal.ReadInt64(p, 8);
            if (pid == self || pid <= 4) continue;
            uint access = (uint)Marshal.ReadInt32(p, 24);
            IntPtr hv = new IntPtr(Marshal.ReadInt64(p, 16));
            IntPtr proc;
            if (!procs.TryGetValue(pid, out proc))
            {
                proc = OpenProcess(0x0040, false, pid);                      // PROCESS_DUP_HANDLE
                procs[pid] = proc;
            }
            if (proc == IntPtr.Zero) continue;
            IntPtr dup;
            if (!DuplicateHandle(proc, hv, GetCurrentProcess(), out dup, 0, false, 2)) continue;   // DUPLICATE_SAME_ACCESS
            try
            {
                string name = NameOf(dup);
                if (name != null && name.IndexOf(match, StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    string pn = "?";
                    try { pn = Process.GetProcessById(pid).ProcessName; } catch { }
                    found.Add(pid + " " + pn + "  " + name);
                }
            }
            finally { CloseHandle(dup); }
        }
        Marshal.FreeHGlobal(buf);
        return found;
    }
}
"@

Write-Host "Programs with a handle whose name contains '$Match':"
$r = [HandleFinder]::Find($Match)
if ($r.Count -eq 0) { Write-Host "   none found (try an elevated PowerShell, or unplug and replug the USB cable)" }
else { $r | ForEach-Object { Write-Host "   $_" } }
[Environment]::Exit(0)
