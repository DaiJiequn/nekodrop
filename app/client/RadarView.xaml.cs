// SPDX-License-Identifier: GPL-2.0-only
using System.Collections.Specialized;
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;

namespace NekoDrop;

/// <summary>
/// Nearby AirDrop receivers, shared by the main window and the send window:
/// a radar (this PC in the middle, devices on a ring around it) or a list.
/// A click on a device sends the chosen files; files dropped on a device are
/// sent to it.
///
/// Everything round is placed from one RadarGeometry, and every ripple
/// scales about its own center from a transparent start, so nothing on the
/// radar can come loose from the middle (the HTML mock-up drew a delayed
/// ripple at full size, off to the lower right, before its animation began).
/// </summary>
public partial class RadarView : UserControl
{
    static App A => App.Instance;

    readonly Ellipse _inner = Ring(), _orbit = Ring(), _outer = Ring();
    readonly Ellipse[] _ripples = [NewRipple(), NewRipple()];
    readonly Grid _hub = new();
    readonly TextBlock _hubLabel = new();
    bool _rippling;

    public RadarView()
    {
        InitializeComponent();
        _outer.Opacity = 0.6;
        foreach (var r in _ripples) Rings.Children.Add(r);
        Rings.Children.Add(_outer);
        Rings.Children.Add(_orbit);
        Rings.Children.Add(_inner);
        BuildHub();
        Rings.Children.Add(_hub);
        Rings.Children.Add(_hubLabel);

        RadarItems.ItemsSource = A.Model.Devices;
        ListItems.ItemsSource = A.Model.Devices;
        (A.Settings.DeviceView == "list" ? ListChoice : RadarChoice).IsChecked = true;

        Loaded += (_, _) =>
        {
            A.Model.Devices.CollectionChanged += OnDevices;
            A.Model.PropertyChanged += OnModel;
            Update();
        };
        Unloaded += (_, _) =>
        {
            A.Model.Devices.CollectionChanged -= OnDevices;
            A.Model.PropertyChanged -= OnModel;
            Ripple(false);
        };
        IsVisibleChanged += (_, _) => Update();
    }

    static Ellipse Ring() => new()
    {
        StrokeThickness = 1,
        Fill = Brushes.Transparent,
    };

    static Ellipse NewRipple() => new()
    {
        StrokeThickness = 1.5,
        Opacity = 0, // invisible until its animation starts
        RenderTransformOrigin = new Point(0.5, 0.5),
        RenderTransform = new ScaleTransform(0.2, 0.2),
    };

    void BuildHub()
    {
        _hub.Width = _hub.Height = RadarGeometry.Hub;
        var disc = new Ellipse { StrokeThickness = 1.5 };
        disc.SetResourceReference(Shape.FillProperty, "AccentSubtle");
        disc.SetResourceReference(Shape.StrokeProperty, "Accent");
        var icon = new TextBlock
        {
            Text = "", // PC
            FontSize = 22,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        icon.SetResourceReference(TextBlock.FontFamilyProperty, "IconFont");
        icon.SetResourceReference(TextBlock.ForegroundProperty, "AccentText");
        _hub.Children.Add(disc);
        _hub.Children.Add(icon);
        _hubLabel.Text = L.T("radar.thisPc");
        _hubLabel.FontSize = 11;
        _hubLabel.TextAlignment = TextAlignment.Center;
        _hubLabel.Width = 120;
        _hubLabel.SetResourceReference(TextBlock.ForegroundProperty, "TextSecondary");
        foreach (var e in new[] { _inner, _orbit, _outer }) e.SetResourceReference(Shape.StrokeProperty, "RingStroke");
        foreach (var r in _ripples) r.SetResourceReference(Shape.StrokeProperty, "Accent");
    }

    void OnDevices(object? s, NotifyCollectionChangedEventArgs e) => Update();

    void OnModel(object? s, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(AppModel.RadioOn) or nameof(AppModel.Status) or nameof(AppModel.Detail)) Update();
    }

    void View_Checked(object sender, RoutedEventArgs e)
    {
        if (!IsLoaded) return;
        A.Settings.DeviceView = ListChoice.IsChecked == true ? "list" : "radar";
        A.Settings.Save();
        Update();
    }

    /// <summary>More devices than the radar holds always show as the list.</summary>
    bool ShowList => ListChoice.IsChecked == true || A.Model.Devices.Count > RadarGeometry.MaxOnRadar;

    void Update()
    {
        var list = ShowList;
        RadarLayer.Visibility = list ? Visibility.Collapsed : Visibility.Visible;
        ListLayer.Visibility = list ? Visibility.Visible : Visibility.Collapsed;
        RadarChoice.IsEnabled = A.Model.Devices.Count <= RadarGeometry.MaxOnRadar;

        var none = A.Model.Devices.Count == 0;
        var on = A.Model.RadioOn;
        var title = on ? L.T("radar.looking") : A.Model.Status;
        var text = on ? L.T("radar.lookingHint") : A.Model.Detail;
        HintTitle.Text = ListHintTitle.Text = title;
        HintText.Text = ListHintText.Text = text;
        HintText.Visibility = ListHintText.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
        Hint.Visibility = ListHint.Visibility = none ? Visibility.Visible : Visibility.Collapsed;

        // Rings are dashed while AirDrop is not listening.
        var dash = on ? null : new DoubleCollection { 4, 4 };
        foreach (var ring in new[] { _inner, _orbit, _outer }) ring.StrokeDashArray = dash;
        Ripple(on && !list && IsVisible && !Motion.Reduced);
    }

    void RadarLayer_SizeChanged(object sender, SizeChangedEventArgs e) => Place(e.NewSize);

    void Place(Size size)
    {
        var g = RadarGeometry.For(size);
        void Circle(FrameworkElement el, double r)
        {
            el.Width = el.Height = 2 * r;
            Canvas.SetLeft(el, g.Center.X - r);
            Canvas.SetTop(el, g.Center.Y - r);
        }
        Circle(_inner, g.Inner);
        Circle(_orbit, g.Orbit);
        Circle(_outer, g.Outer);
        foreach (var r in _ripples) Circle(r, g.Outer);
        Circle(_hub, RadarGeometry.Hub / 2);
        Canvas.SetLeft(_hubLabel, g.Center.X - _hubLabel.Width / 2);
        Canvas.SetTop(_hubLabel, g.Center.Y + RadarGeometry.Hub / 2 + 4);
    }

    /// <summary>
    /// Two rings that grow from the hub and fade, half a period apart. Each
    /// starts transparent and small, so the second one shows nothing during
    /// its delay.
    /// </summary>
    void Ripple(bool on)
    {
        if (on == _rippling) return;
        _rippling = on;
        var period = TimeSpan.FromSeconds(3.2);
        for (var i = 0; i < _ripples.Length; i++)
        {
            var r = _ripples[i];
            var scale = (ScaleTransform)r.RenderTransform;
            if (!on)
            {
                r.BeginAnimation(OpacityProperty, null);
                scale.BeginAnimation(ScaleTransform.ScaleXProperty, null);
                scale.BeginAnimation(ScaleTransform.ScaleYProperty, null);
                r.Opacity = 0;
                continue;
            }
            var delay = TimeSpan.FromTicks(period.Ticks * i / _ripples.Length);
            var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
            DoubleAnimation Grow() => new(0.2, 1, period)
            {
                BeginTime = delay, RepeatBehavior = RepeatBehavior.Forever, EasingFunction = ease,
                FillBehavior = FillBehavior.Stop,
            };
            scale.BeginAnimation(ScaleTransform.ScaleXProperty, Grow());
            scale.BeginAnimation(ScaleTransform.ScaleYProperty, Grow());
            r.BeginAnimation(OpacityProperty, new DoubleAnimation(0.45, 0, period)
            {
                BeginTime = delay, RepeatBehavior = RepeatBehavior.Forever, FillBehavior = FillBehavior.Stop,
            });
        }
    }

    // ---------------------------------------------------------------- sending

    void Node_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is Device d) Send(d, null);
    }

    void Node_DragEnter(object sender, DragEventArgs e) => Highlight(sender, Accepts(e));
    void Node_DragLeave(object sender, DragEventArgs e) => Highlight(sender, false);

    void Node_DragOver(object sender, DragEventArgs e)
    {
        e.Effects = Accepts(e) ? DragDropEffects.Copy : DragDropEffects.None;
        e.Handled = true;
    }

    void Node_Drop(object sender, DragEventArgs e)
    {
        Highlight(sender, false);
        // Handled: the window's own drop handler would add the files to the list as well.
        e.Handled = true;
        if ((sender as FrameworkElement)?.Tag is Device d && e.Data.GetData(DataFormats.FileDrop) is string[] files)
            Send(d, files);
    }

    static bool Accepts(DragEventArgs e) =>
        e.Data.GetDataPresent(DataFormats.FileDrop) && (e.Source as FrameworkElement)?.DataContext is Device { Busy: false };

    /// <summary>Files dropped on a device go to it alone; a click sends the files in the list (or asks for some).</summary>
    void Send(Device d, string[]? dropped)
    {
        if (d.Busy) return;
        var files = A.Model.Files;
        if (dropped is { Length: > 0 })
        {
            files.Clear();
            foreach (var f in dropped) files.Add(f);
        }
        if (files.Count == 0)
        {
            var dlg = new Microsoft.Win32.OpenFileDialog { Multiselect = true, Title = L.T("pick.files") };
            if (dlg.ShowDialog(Window.GetWindow(this)) != true) return;
            foreach (var f in dlg.FileNames) files.Add(f);
        }
        A.SendTo(d);
    }

    static void Highlight(object sender, bool on)
    {
        if (sender is not DependencyObject root || Find(root, "Disc") is not Ellipse disc) return;
        if (on)
        {
            disc.SetResourceReference(Shape.StrokeProperty, "Accent");
            disc.StrokeThickness = 2;
        }
        else
        {
            disc.SetResourceReference(Shape.StrokeProperty, "CardStroke");
            disc.StrokeThickness = 1;
        }
    }

    static FrameworkElement? Find(DependencyObject root, string name)
    {
        for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is FrameworkElement fe && fe.Name == name) return fe;
            if (Find(child, name) is { } found) return found;
        }
        return null;
    }
}
