using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace FogConsole.Controls;

/// <summary>
/// Lets every scrolling area of the application be scrolled with a finger on a touch screen (drag, with inertia).
///
/// WPF leaves <see cref="ScrollViewer.PanningMode"/> at <c>None</c>, so a touch drag does nothing. The mode is switched on the moment a
/// finger lands on a ScrollViewer, before the ScrollViewer looks at it (switching it on when the control loads is not reliable for
/// scroll viewers that are created later from templates). Pages scroll vertically only; a ScrollViewer with a horizontal scroll bar
/// pans both ways.
///
/// Buttons that need the finger for themselves (the press-and-hold buttons, <see cref="HoldBehavior"/>) take the touch before the
/// page can start a pan; sliders and text boxes keep working as they do with a mouse.
/// </summary>
public static class TouchScroll
{
    private static bool _installed;

    /// <summary>Call once at start-up (before the first window is shown).</summary>
    public static void Install()
    {
        if (_installed) return;
        _installed = true;
        EventManager.RegisterClassHandler(typeof(ScrollViewer), UIElement.PreviewTouchDownEvent, new EventHandler<TouchEventArgs>(OnPreviewTouchDown));
    }

    private static void OnPreviewTouchDown(object? sender, TouchEventArgs e)
    {
        if (sender is not ScrollViewer sv || sv.PanningMode != PanningMode.None) return;
        bool vertical = sv.VerticalScrollBarVisibility != ScrollBarVisibility.Disabled;
        bool horizontal = sv.HorizontalScrollBarVisibility != ScrollBarVisibility.Disabled;

        // A ScrollViewer that cannot scroll at all (the one inside a list that sits in a page) must not take the finger: it would
        // become the target of the drag and the page behind it would never move.
        if (vertical && horizontal) sv.PanningMode = PanningMode.Both;
        else if (vertical) sv.PanningMode = PanningMode.VerticalOnly;
        else if (horizontal) sv.PanningMode = PanningMode.HorizontalOnly;
    }
}
