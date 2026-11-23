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
/// a radar (this PC in the middle, devices on a ring around it) or a list,
/// and in their place what keeps AirDrop from running and what to do about it.
/// A click on a device sends the chosen files; files dropped on a device are
/// sent to it.
///
/// Everything round is placed from one RadarGeometry, and every ripple
/// scales about its own center from a transparent start, so nothing on the
/// radar can come loose from the middle.
/// </summary>
public partial class RadarView : UserControl
{
    static App A => App.Instance;

    readonly Ellipse _inner = Ring(), _orbit = Ring();
    readonly Ellipse[] _ripples = [NewRipple(), NewRipple()];
    readonly Grid _hub = new();
    readonly Ellipse _hubSpinner = new() { StrokeThickness = 2.5, StrokeDashArray = [2.4, 2.4], Visibility = Visibility.Collapsed };
    readonly TextBlock _hubLabel = new();
    // A soft light behind the hub that breathes while AirDrop listens.
    readonly Ellipse _halo = new() { RenderTransformOrigin = new Point(0.5, 0.5), RenderTransform = new ScaleTransform(1, 1), Opacity = 0 };
    readonly HashSet<INotifyPropertyChanged> _watched = [];
    bool _rippling, _breathing, _hintShown;

    public RadarView()
    {
        InitializeComponent();
        foreach (var r in _ripples) Rings.Children.Add(r);
        Rings.Children.Add(_halo);
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
            foreach (var w in _watched) w.PropertyChanged -= OnDevice;
            _watched.Clear();
            Ripple(false);
            Breathe(false);
            Spin(false);
        };
        IsVisibleChanged += (_, _) => Update();
    }

    /// <summary>The send window's smaller heading.</summary>
    public bool Compact { set => Heading.FontSize = value ? 13 : 14; }

    static Ellipse Ring() => new() { StrokeThickness = 1, Fill = Brushes.Transparent };

    static Ellipse NewRipple() => new()
    {
        StrokeThickness = 1.5,
        Opacity = 0, // invisible until its animation starts
        RenderTransformOrigin = new Point(0.5, 0.5),
        RenderTransform = new ScaleTransform(0.26, 0.26),
    };

    void BuildHub()
    {
        _hub.Width = _hub.Height = RadarGeometry.Hub;
        var disc = new Ellipse { StrokeThickness = 1.5 };
        disc.SetResourceReference(Shape.FillProperty, "AccentSubtle");
        disc.SetResourceReference(Shape.StrokeProperty, "Accent");
        var icon = new TextBlock
        {
            Text = HubLaptop() ? "\uE7F8" : "\uE7F4", // a laptop or a desktop monitor
            FontSize = 20,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        icon.SetResourceReference(TextBlock.FontFamilyProperty, "IconFont");
        icon.SetResourceReference(TextBlock.ForegroundProperty, "Accent");
        _hubSpinner.SetResourceReference(Shape.StrokeProperty, "Accent");
        _halo.SetResourceReference(Shape.FillProperty, "AccentSubtle");
        _hubSpinner.RenderTransformOrigin = new Point(0.5, 0.5);
        _hubSpinner.RenderTransform = new RotateTransform();
        _hub.Children.Add(disc);
        _hub.Children.Add(_hubSpinner);
        _hub.Children.Add(icon);
        _hubLabel.Text = L.T("radar.thisPc");
        _hubLabel.FontSize = 11;
        _hubLabel.FontWeight = FontWeights.Medium;
        _hubLabel.TextAlignment = TextAlignment.Center;
        _hubLabel.Width = 120;
        _hubLabel.SetResourceReference(TextBlock.ForegroundProperty, "Text2");
        foreach (var e in new[] { _inner, _orbit }) e.SetResourceReference(Shape.StrokeProperty, "Border");
        _orbit.Opacity = 1;
        _inner.Opacity = 1;
        foreach (var r in _ripples) r.SetResourceReference(Shape.StrokeProperty, "Accent");
    }

    /// <summary>This PC's icon: as set in the settings, or a laptop when Windows reports a battery.</summary>
    static bool HubLaptop() => A.Settings.HubIcon switch
    {
        "laptop" => true,
        "desktop" => false,
        _ => !System.Windows.Forms.SystemInformation.PowerStatus.BatteryChargeStatus.HasFlag(System.Windows.Forms.BatteryChargeStatus.NoSystemBattery),
    };

    void OnDevices(object? s, NotifyCollectionChangedEventArgs e) => Update();

    void OnDevice(object? s, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(Device.State) or nameof(Device.Ask) or nameof(Request.Receiving))
            Dispatcher.BeginInvoke(DrawArrows);
    }

    void OnModel(object? s, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(AppModel.Air) or nameof(AppModel.Detail)) Update();
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
        var air = A.Model.Air;
        var none = A.Model.Devices.Count == 0;
        var blocked = air is "off" or "error" or "service_stopped" or "no_adapter";
        // The list shows devices only; every other state is drawn on the radar.
        var list = !blocked && air == "ready" && !none && ShowList;

        Blocked.Visibility = blocked ? Visibility.Visible : Visibility.Collapsed;
        RadarLayer.Visibility = !blocked && !list ? Visibility.Visible : Visibility.Collapsed;
        ListLayer.Visibility = list ? Visibility.Visible : Visibility.Collapsed;
        RadarChoice.IsEnabled = A.Model.Devices.Count <= RadarGeometry.MaxOnRadar;
        RadarItems.Visibility = air == "ready" ? Visibility.Visible : Visibility.Collapsed;

        if (blocked) ShowBlocked(air);

        // Below the hub: looking (no device yet), standby, starting.
        string title = "", text = "", button = "";
        if (air == "ready" && none) { title = L.T("radar.looking"); text = L.T("radar.lookingHint"); }
        else if (air == "standby") { text = L.T("radar.standby"); button = L.T("radar.wake"); }
        else if (air == "starting") { text = L.T("status.starting"); }
        HintTitle.Text = title;
        HintTitle.Visibility = title == "" ? Visibility.Collapsed : Visibility.Visible;
        HintText.Text = text;
        HintText.Visibility = text == "" ? Visibility.Collapsed : Visibility.Visible;
        HintText.FontSize = title == "" ? 13 : 12;
        HintButton.Content = button;
        HintButton.Visibility = button == "" ? Visibility.Collapsed : Visibility.Visible;
        var hint = title != "" || text != "";
        Hint.Visibility = hint && !blocked ? Visibility.Visible : Visibility.Collapsed;
        if (hint != _hintShown) { _hintShown = hint; Place(new Size(RadarLayer.ActualWidth, RadarLayer.ActualHeight)); }

        // Rings are dashed on standby; the hub's dashed ring turns while starting.
        var dash = air == "standby" ? new DoubleCollection { 4, 4 } : null;
        foreach (var ring in new[] { _inner, _orbit }) ring.StrokeDashArray = dash;
        var visible = IsVisible && !blocked && !list;
        Ripple(air == "ready" && visible && !Motion.Reduced);
        Breathe(air == "ready" && visible && !Motion.Reduced);
        Spin(air == "starting" && visible);
        DrawArrows();
    }

    void ShowBlocked(string air)
    {
        var (glyph, brush, title, text, action, report) = air switch
        {
            "off" => ("\uE7E8", "Text3", L.T("status.off"), L.T("radar.offHint"), L.T("radar.turnOn"), false),
            "error" => ("\uE783", "Error", L.T("status.error"), A.Model.Detail, L.T("radar.retry"), true),
            "service_stopped" => ("\uE783", "Error", L.T("status.noService"), L.T("radar.serviceHint"), L.T("radar.startService"), true),
            _ => ("\uE701", "Warn", L.T("radar.noCard"), L.T("radar.noCardHint"), L.T("radar.chooseCard"), false),
        };
        BlockedGlyph.Text = glyph;
        BlockedGlyph.SetResourceReference(TextBlock.ForegroundProperty, brush);
        BlockedTitle.Text = title;
        BlockedText.Text = text;
        BlockedText.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
        BlockedAction.Content = action;
        BlockedReport.Visibility = report ? Visibility.Visible : Visibility.Collapsed;
    }

    void BlockedAction_Click(object sender, RoutedEventArgs e)
    {
        switch (A.Model.Air)
        {
            case "off": A.Service.SetMode("concurrent"); break;
            case "error": A.Service.Wake(); break;
            case "service_stopped": App.StartService(); break;
            default: A.ShowSettings("hardware"); break;
        }
    }

    void HintButton_Click(object sender, RoutedEventArgs e) => A.Service.Wake();
    void Accept_Click(object sender, RoutedEventArgs e) { if ((sender as FrameworkElement)?.Tag is string id) A.Answer(id, true); }
    void Decline_Click(object sender, RoutedEventArgs e) { if ((sender as FrameworkElement)?.Tag is string id) A.Answer(id, false); }
    void Report_Click(object sender, RoutedEventArgs e) => App.RunReport();

    void RadarLayer_SizeChanged(object sender, SizeChangedEventArgs e) => Place(e.NewSize);

    void Place(Size size)
    {
        if (size.Width <= 0 || size.Height <= 0) return;
        // A hint below the hub lifts it to 40 % of the height, clear of the text.
        var g = RadarGeometry.For(size);
        var center = _hintShown ? new Point(g.Center.X, size.Height * 0.40) : g.Center;
        void Circle(FrameworkElement el, double r)
        {
            el.Width = el.Height = 2 * r;
            Canvas.SetLeft(el, center.X - r);
            Canvas.SetTop(el, center.Y - r);
        }
        Circle(_inner, g.Inner);
        Circle(_orbit, g.Orbit);
        foreach (var r in _ripples) Circle(r, g.Outer);
        Circle(_hub, RadarGeometry.Hub / 2);
        Canvas.SetLeft(_hubLabel, center.X - _hubLabel.Width / 2);
        Canvas.SetTop(_hubLabel, center.Y + RadarGeometry.Hub / 2 + 4);
        Circle(_halo, RadarGeometry.Hub / 2 + 14);
        DrawArrows();
    }

    /// <summary>
    /// The hub's halo swells and fades, and the guide rings brighten with it:
    /// a slow breath (1.8 s each way) while AirDrop listens.
    /// </summary>
    void Breathe(bool on)
    {
        if (on == _breathing) return;
        _breathing = on;
        var scale = (ScaleTransform)_halo.RenderTransform;
        if (!on)
        {
            scale.BeginAnimation(ScaleTransform.ScaleXProperty, null);
            scale.BeginAnimation(ScaleTransform.ScaleYProperty, null);
            _halo.BeginAnimation(OpacityProperty, null);
            foreach (var ring in new[] { _inner, _orbit }) ring.BeginAnimation(OpacityProperty, null);
            _halo.Opacity = 0;
            return;
        }
        var half = TimeSpan.FromSeconds(1.8);
        var ease = new SineEase { EasingMode = EasingMode.EaseInOut };
        DoubleAnimation Breath(double from, double to) =>
            new(from, to, half) { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever, EasingFunction = ease };
        scale.BeginAnimation(ScaleTransform.ScaleXProperty, Breath(0.86, 1.14));
        scale.BeginAnimation(ScaleTransform.ScaleYProperty, Breath(0.86, 1.14));
        _halo.BeginAnimation(OpacityProperty, Breath(0.95, 0.35));
        foreach (var ring in new[] { _inner, _orbit }) ring.BeginAnimation(OpacityProperty, Breath(0.65, 1));
    }

    /// <summary>
    /// A line from this PC to each device it sends to (from the device when it
    /// sends to us), with a small chevron at the head: flowing dots while the
    /// other side has not answered, solid while files move, green once sent,
    /// grey when declined or failed.
    /// </summary>
    void DrawArrows()
    {
        Arrows.Children.Clear();
        foreach (var d in A.Model.Devices)
        {
            if (_watched.Add(d)) d.PropertyChanged += OnDevice;
            if (d.Ask is { } r && _watched.Add(r)) r.PropertyChanged += OnDevice;
        }
        var size = new Size(RadarLayer.ActualWidth, RadarLayer.ActualHeight);
        if (size.Width <= 0 || A.Model.Air != "ready" || RadarLayer.Visibility != Visibility.Visible) return;
        var g = RadarGeometry.For(size);
        var n = A.Model.Devices.Count;
        for (var i = 0; i < n; i++)
        {
            var d = A.Model.Devices[i];
            var (incoming, waiting, brush) = d.Ask is { } ask
                ? (true, !ask.Receiving, "Accent")
                : d.State switch
                {
                    "asking" => (false, true, "Accent"),
                    "sending" => (false, false, "Accent"),
                    "sent" => (false, false, "Success"),
                    "declined" or "failed" => (false, false, "Text3"),
                    _ => (false, false, ""),
                };
            if (brush == "") continue;
            var icon = g.IconCenter(i, n);
            var v = icon - g.Center;
            v.Normalize();
            var near = g.Center + v * (RadarGeometry.Hub / 2 + 8);
            var far = icon - v * (RadarGeometry.Node / 2 + 8);
            var (tail, head) = incoming ? (far, near) : (near, far);
            var dim = brush == "Text3";

            var line = new Line { X1 = tail.X, Y1 = tail.Y, X2 = head.X, Y2 = head.Y, StrokeThickness = 2, Opacity = dim ? 0.55 : 0.9,
                                  StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round };
            line.SetResourceReference(Shape.StrokeProperty, brush);
            if (waiting || dim)
            {
                line.StrokeDashCap = PenLineCap.Round;
                line.StrokeDashArray = [0, 3]; // dots 6 px apart
                if (waiting && !Motion.Reduced)
                    line.BeginAnimation(Shape.StrokeDashOffsetProperty,
                        new DoubleAnimation(0, -3, TimeSpan.FromSeconds(0.45)) { RepeatBehavior = RepeatBehavior.Forever });
            }
            Arrows.Children.Add(line);

            // The chevron: two 7 px strokes back from the head at 32 degrees.
            var back = tail - head;
            back.Normalize();
            Vector Turn(Vector u, double deg)
            {
                var a = deg * Math.PI / 180;
                return new Vector(u.X * Math.Cos(a) - u.Y * Math.Sin(a), u.X * Math.Sin(a) + u.Y * Math.Cos(a));
            }
            var chevron = new System.Windows.Shapes.Path
            {
                Data = new PathGeometry([new PathFigure(head + Turn(back, 32) * 7, [new LineSegment(head, true), new LineSegment(head + Turn(back, -32) * 7, true)], false)]),
                StrokeThickness = 2, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round,
                StrokeLineJoin = PenLineJoin.Round, Opacity = line.Opacity,
            };
            chevron.SetResourceReference(Shape.StrokeProperty, brush);
            Arrows.Children.Add(chevron);
        }
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
        var period = TimeSpan.FromSeconds(2.8);
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
            DoubleAnimation Grow() => new(0.26, 1, period)
            {
                BeginTime = delay, RepeatBehavior = RepeatBehavior.Forever, EasingFunction = ease,
            };
            scale.BeginAnimation(ScaleTransform.ScaleXProperty, Grow());
            scale.BeginAnimation(ScaleTransform.ScaleYProperty, Grow());
            r.BeginAnimation(OpacityProperty, new DoubleAnimation(0.26, 0, period)
            {
                BeginTime = delay, RepeatBehavior = RepeatBehavior.Forever, EasingFunction = ease,
            });
        }
    }

    void Spin(bool on)
    {
        _hubSpinner.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        var turn = (RotateTransform)_hubSpinner.RenderTransform;
        turn.BeginAnimation(RotateTransform.AngleProperty, on && !Motion.Reduced
            ? new DoubleAnimation(0, 360, TimeSpan.FromSeconds(1.6)) { RepeatBehavior = RepeatBehavior.Forever }
            : null);
    }

    // ---------------------------------------------------------------- sending

    void Node_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is Device d) Send(d, null);
    }

    void Node_DragEnter(object sender, DragEventArgs e) => Highlight(sender, Accepts(sender, e));
    void Node_DragLeave(object sender, DragEventArgs e) => Highlight(sender, false);

    void Node_DragOver(object sender, DragEventArgs e)
    {
        e.Effects = Accepts(sender, e) ? DragDropEffects.Copy : DragDropEffects.None;
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

    static bool Accepts(object sender, DragEventArgs e) =>
        e.Data.GetDataPresent(DataFormats.FileDrop) && (sender as FrameworkElement)?.Tag is Device { Busy: false };

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

    /// <summary>A file dragged over a device: it grows a little, with an accent halo (radar) or outline (list).</summary>
    static void Highlight(object sender, bool on)
    {
        if (sender is not FrameworkElement root) return;
        if (root is Border row)
        {
            row.SetResourceReference(Border.BorderBrushProperty, on ? "Accent" : "Border");
            return;
        }
        if (Find(root, "Glow") is UIElement glow) glow.Visibility = on ? Visibility.Visible : Visibility.Hidden;
        if (Find(root, "Disc") is Ellipse disc) disc.SetResourceReference(Shape.StrokeProperty, on ? "Accent" : "BorderStrong");
        if (root.RenderTransform is TransformGroup { Children: [ScaleTransform grow, ..] })
        {
            var to = on ? 1.09 : 1.0;
            grow.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimation(to, TimeSpan.FromMilliseconds(150)));
            grow.BeginAnimation(ScaleTransform.ScaleYProperty, new DoubleAnimation(to, TimeSpan.FromMilliseconds(150)));
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
