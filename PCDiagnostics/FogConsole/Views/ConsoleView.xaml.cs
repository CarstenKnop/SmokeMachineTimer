using System.Windows.Controls;
using System.Windows.Input;
using FogConsole.ViewModels;

namespace FogConsole.Views;

public partial class ConsoleView : UserControl
{
    public ConsoleView() => InitializeComponent();

    /// <summary>Command history in the raw command box (pure input handling; the history itself lives in the view model).</summary>
    private void RawBox_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (DataContext is not ConsoleViewModel vm) return;
        if (e.Key == Key.Up) { vm.HistoryUp(); e.Handled = true; MoveCaretToEnd(); }
        else if (e.Key == Key.Down) { vm.HistoryDown(); e.Handled = true; MoveCaretToEnd(); }
    }

    private void MoveCaretToEnd()
    {
        RawBox.CaretIndex = RawBox.Text.Length;
    }
}
