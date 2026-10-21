using System.IO;
using System.Windows;
using FogConsole.Views;
using Microsoft.Win32;

namespace FogConsole.Services;

public interface IDialogService
{
    /// <summary>Asks for confirmation. Returns true when the user confirmed.</summary>
    bool Confirm(string title, string message, string confirmText, bool destructive = false);

    /// <summary>Returns the chosen path or null.</summary>
    string? PickSaveFile(string title, string filter, string defaultFileName);

    void CopyText(string text);

    void SaveText(string path, string text);
}

public sealed class WpfDialogService : IDialogService
{
    public bool Confirm(string title, string message, string confirmText, bool destructive = false)
    {
        var dlg = new ConfirmDialog(title, message, confirmText, destructive);
        var owner = Application.Current?.Windows.OfType<Window>().FirstOrDefault(w => w.IsActive) ?? Application.Current?.MainWindow;
        if (owner is not null && owner.IsVisible && !ReferenceEquals(owner, dlg)) dlg.Owner = owner;
        return dlg.ShowDialog() == true;
    }

    public string? PickSaveFile(string title, string filter, string defaultFileName)
    {
        var dlg = new SaveFileDialog { Title = title, Filter = filter, FileName = defaultFileName, OverwritePrompt = true };
        return dlg.ShowDialog() == true ? dlg.FileName : null;
    }

    public void CopyText(string text)
    {
        for (int i = 0; i < 5; i++)
        {
            try
            {
                Clipboard.SetText(text);
                return;
            }
            catch (System.Runtime.InteropServices.COMException)
            {
                Thread.Sleep(40); // clipboard is locked by another process
            }
        }
    }

    public void SaveText(string path, string text) => File.WriteAllText(path, text, new System.Text.UTF8Encoding(false));
}

/// <summary>Never shows anything and answers with a preset value (unit tests, screenshot mode).</summary>
public sealed class ScriptedDialogService : IDialogService
{
    public bool ConfirmAnswer { get; set; } = true;
    public List<string> Asked { get; } = new();
    public string? Clipboard { get; private set; }
    public string? SavePath { get; set; }
    public Dictionary<string, string> Saved { get; } = new();

    public bool Confirm(string title, string message, string confirmText, bool destructive = false)
    {
        Asked.Add(title + ": " + message);
        return ConfirmAnswer;
    }

    public string? PickSaveFile(string title, string filter, string defaultFileName) => SavePath;
    public void CopyText(string text) => Clipboard = text;
    public void SaveText(string path, string text) => Saved[path] = text;
}
