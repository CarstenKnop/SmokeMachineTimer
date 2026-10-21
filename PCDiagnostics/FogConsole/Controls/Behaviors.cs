using System.Collections.Specialized;
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;

namespace FogConsole.Controls;

/// <summary>Small view-only behaviours that cannot be expressed in XAML.</summary>
public static class Behaviors
{
    // ---- focus a text box when it becomes visible (inline editing) --------------------------------

    public static readonly DependencyProperty FocusWhenVisibleProperty = DependencyProperty.RegisterAttached("FocusWhenVisible", typeof(bool), typeof(Behaviors),
        new PropertyMetadata(false, (d, e) =>
        {
            if (d is not UIElement el) return;
            el.IsVisibleChanged -= OnVisibleChanged;
            if (e.NewValue is true) el.IsVisibleChanged += OnVisibleChanged;
        }));

    public static bool GetFocusWhenVisible(DependencyObject d) => (bool)d.GetValue(FocusWhenVisibleProperty);
    public static void SetFocusWhenVisible(DependencyObject d, bool v) => d.SetValue(FocusWhenVisibleProperty, v);

    private static void OnVisibleChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (sender is not UIElement el || e.NewValue is not true) return;
        el.Dispatcher.BeginInvoke(DispatcherPriority.Input, () =>
        {
            el.Focus();
            Keyboard.Focus(el);
            if (el is TextBox tb) tb.SelectAll();
        });
    }

    // ---- scroll an items control to the newest item ------------------------------------------------

    public static readonly DependencyProperty AutoScrollProperty = DependencyProperty.RegisterAttached("AutoScroll", typeof(bool), typeof(Behaviors),
        new PropertyMetadata(false, OnAutoScrollChanged));

    public static bool GetAutoScroll(DependencyObject d) => (bool)d.GetValue(AutoScrollProperty);
    public static void SetAutoScroll(DependencyObject d, bool v) => d.SetValue(AutoScrollProperty, v);

    private static readonly DependencyProperty HookProperty = DependencyProperty.RegisterAttached("Hook", typeof(NotifyCollectionChangedEventHandler), typeof(Behaviors));
    private static readonly DependencyProperty HookedSourceProperty = DependencyProperty.RegisterAttached("HookedSource", typeof(INotifyCollectionChanged), typeof(Behaviors));

    private static void OnAutoScrollChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not ItemsControl ic) return;
        ic.Loaded -= OnItemsControlLoaded;
        if (e.NewValue is true)
        {
            ic.Loaded += OnItemsControlLoaded;
            if (ic.IsLoaded) Attach(ic);
        }
        else
        {
            Detach(ic);
        }
    }

    private static void OnItemsControlLoaded(object sender, RoutedEventArgs e) => Attach((ItemsControl)sender);

    private static void Attach(ItemsControl ic)
    {
        Detach(ic);
        if (ic.ItemsSource is INotifyCollectionChanged src)
        {
            NotifyCollectionChangedEventHandler handler = (_, _) => ScrollToEnd(ic);
            src.CollectionChanged += handler;
            ic.SetValue(HookProperty, handler);
            ic.SetValue(HookedSourceProperty, src);
        }
        DependencyPropertyDescriptor.FromProperty(ItemsControl.ItemsSourceProperty, typeof(ItemsControl))?.AddValueChanged(ic, OnItemsSourceChanged);
    }

    private static void OnItemsSourceChanged(object? sender, EventArgs e)
    {
        if (sender is ItemsControl ic && GetAutoScroll(ic)) Attach(ic);
    }

    private static void Detach(ItemsControl ic)
    {
        if (ic.GetValue(HookedSourceProperty) is INotifyCollectionChanged src && ic.GetValue(HookProperty) is NotifyCollectionChangedEventHandler h)
            src.CollectionChanged -= h;
        ic.ClearValue(HookProperty);
        ic.ClearValue(HookedSourceProperty);
    }

    private static void ScrollToEnd(ItemsControl ic)
    {
        if (!GetAutoScroll(ic)) return;
        ic.Dispatcher.BeginInvoke(DispatcherPriority.Background, () =>
        {
            if (ic.Items.Count == 0) return;
            var last = ic.Items[ic.Items.Count - 1];
            switch (ic)
            {
                case ListBox lb: lb.ScrollIntoView(last); break;
                case DataGrid dg: dg.ScrollIntoView(last); break;
            }
        });
    }
}
