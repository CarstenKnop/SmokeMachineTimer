using System.Windows;
using System.Windows.Media;

namespace FogConsole.Views;

public partial class ConfirmDialog : Window
{
    public ConfirmDialog(string title, string message, string confirmText, bool destructive)
    {
        InitializeComponent();
        Title = title;
        TitleText.Text = title;
        MessageText.Text = message;
        ConfirmButton.Content = confirmText;
        IconText.Text = destructive ? "" : "";
        IconText.SetResourceReference(ForegroundProperty, destructive ? "Fog.Bad" : "Fog.Accent");
        IconText.SetResourceReference(System.Windows.Controls.TextBlock.ForegroundProperty, destructive ? "Fog.Bad" : "Fog.Accent");
        if (destructive)
        {
            ConfirmButton.Background = (Brush)FindResource("Fog.Danger");
            ConfirmButton.Foreground = Brushes.White;
            ConfirmButton.BorderBrush = ConfirmButton.Background;
        }
        else
        {
            ConfirmButton.Style = (Style)FindResource("AccentButtonStyle");
        }
    }

    private void Confirm_Click(object sender, RoutedEventArgs e)
    {
        DialogResult = true;
    }
}
