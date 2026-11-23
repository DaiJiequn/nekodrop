// SPDX-License-Identifier: GPL-2.0-only
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace NekoDrop;

/// <summary>
/// One geometry for everything on the radar, so the rings, the ripples, the
/// center and the devices can never drift apart: they all come from the size
/// of the stage. Devices sit with their icon centered on the middle ring.
/// </summary>
public readonly record struct RadarGeometry(Point Center, double Orbit, double Inner, double Outer)
{
    /// <summary>A device's icon (its circle), in pixels.</summary>
    public const double Node = 50;
    /// <summary>A device's whole box: 4 px padding, icon, name and status below it.</summary>
    public const double NodeWidth = 112, NodeHeight = 4 + Node + 4 + 16 + 15 + 4;
    /// <summary>"This PC" in the middle.</summary>
    public const double Hub = 58;
    /// <summary>More devices than this go to the list.</summary>
    public const int MaxOnRadar = 8;

    public static RadarGeometry For(Size stage)
    {
        var center = new Point(stage.Width / 2, stage.Height / 2);
        // The orbit keeps every device box inside the stage. A box hangs its name
        // below the icon, so the lowest device needs more room than the top one;
        // no count up to MaxOnRadar puts a device lower than sin = 0.93 (eight
        // devices, the pair at 67.5 degrees), and only odd counts put one at the top.
        var byWidth = stage.Width / 2 - NodeWidth / 2 - 8;
        var byTop = stage.Height / 2 - Node / 2 - 8;
        var byBottom = (stage.Height / 2 - (NodeHeight - Node / 2) - 8) / 0.93;
        // Never so small that the icons touch the hub.
        var orbit = Math.Max(Hub / 2 + Node / 2 + 12, Math.Min(byWidth, Math.Min(byTop, byBottom)));
        // Two guide rings as in the design (154 and 248 px there): the orbit and one inside it.
        return new RadarGeometry(center, orbit, orbit * 0.62, orbit * 1.05);
    }

    /// <summary>
    /// Where device i of n goes, as an angle: evenly spread, the first at the
    /// top for odd counts and the pair at the sides for even ones, so the
    /// picture is always symmetric left to right.
    /// </summary>
    public static double Angle(int i, int n) =>
        -Math.PI / 2 + 2 * Math.PI * i / n + (n % 2 == 0 ? Math.PI / n : 0);

    public Point IconCenter(int i, int n)
    {
        if (n == 1) return new Point(Center.X, Center.Y - Orbit);
        var a = Angle(i, n);
        return new Point(Center.X + Orbit * Math.Cos(a), Center.Y + Orbit * Math.Sin(a));
    }
}

/// <summary>Puts its children (device boxes) on the radar's orbit.</summary>
public sealed class RadarPanel : Panel
{
    /// <summary>
    /// Set on each child: it sits in the lower half, so its request bubble
    /// opens above the icon instead of below the name (it would leave the stage).
    /// </summary>
    public static readonly DependencyProperty LowerProperty = DependencyProperty.RegisterAttached(
        "Lower", typeof(bool), typeof(RadarPanel), new FrameworkPropertyMetadata(false));
    public static bool GetLower(DependencyObject d) => (bool)d.GetValue(LowerProperty);
    public static void SetLower(DependencyObject d, bool value) => d.SetValue(LowerProperty, value);

    protected override Size MeasureOverride(Size available)
    {
        foreach (UIElement c in InternalChildren)
            c.Measure(new Size(RadarGeometry.NodeWidth, RadarGeometry.NodeHeight));
        return new Size(double.IsInfinity(available.Width) ? 0 : available.Width,
                        double.IsInfinity(available.Height) ? 0 : available.Height);
    }

    protected override Size ArrangeOverride(Size final)
    {
        var g = RadarGeometry.For(final);
        var n = InternalChildren.Count;
        for (var i = 0; i < n; i++)
        {
            var p = g.IconCenter(i, n);
            SetLower(InternalChildren[i], p.Y > g.Center.Y + 1);
            // The icon is at the top of the box: its center is padding and half an icon down.
            InternalChildren[i].Arrange(new Rect(p.X - RadarGeometry.NodeWidth / 2, p.Y - 4 - RadarGeometry.Node / 2,
                                                 RadarGeometry.NodeWidth, RadarGeometry.NodeHeight));
        }
        return final;
    }
}

/// <summary>
/// A ring around a device's icon: the share sent so far as an arc from the
/// top, clockwise; while nothing is sent yet (waiting for the other side to
/// accept) a short arc turns round instead.
/// </summary>
public sealed class ProgressRing : FrameworkElement
{
    public static readonly DependencyProperty ProgressProperty = DependencyProperty.Register(
        nameof(Progress), typeof(double), typeof(ProgressRing),
        new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender, (d, _) => ((ProgressRing)d).Spin()));
    public static readonly DependencyProperty StrokeProperty = DependencyProperty.Register(
        nameof(Stroke), typeof(Brush), typeof(ProgressRing),
        new FrameworkPropertyMetadata(Brushes.DodgerBlue, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty TrackProperty = DependencyProperty.Register(
        nameof(Track), typeof(Brush), typeof(ProgressRing),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty ThicknessProperty = DependencyProperty.Register(
        nameof(Thickness), typeof(double), typeof(ProgressRing),
        new FrameworkPropertyMetadata(3.0, FrameworkPropertyMetadataOptions.AffectsRender));

    public double Progress { get => (double)GetValue(ProgressProperty); set => SetValue(ProgressProperty, value); }
    public Brush Stroke { get => (Brush)GetValue(StrokeProperty); set => SetValue(StrokeProperty, value); }
    public Brush? Track { get => (Brush?)GetValue(TrackProperty); set => SetValue(TrackProperty, value); }
    public double Thickness { get => (double)GetValue(ThicknessProperty); set => SetValue(ThicknessProperty, value); }

    readonly RotateTransform _turn = new();

    public ProgressRing()
    {
        RenderTransform = _turn;
        RenderTransformOrigin = new Point(0.5, 0.5);
        IsVisibleChanged += (_, _) => Spin();
        Loaded += (_, _) => Spin();
    }

    bool Waiting => Progress <= 0;

    void Spin()
    {
        if (Waiting && IsVisible && !Motion.Reduced)
        {
            if (_turn.HasAnimatedProperties) return;
            _turn.BeginAnimation(RotateTransform.AngleProperty,
                new DoubleAnimation(0, 360, TimeSpan.FromSeconds(1.2)) { RepeatBehavior = RepeatBehavior.Forever });
        }
        else
        {
            _turn.BeginAnimation(RotateTransform.AngleProperty, null);
            _turn.Angle = 0;
        }
    }

    protected override void OnRender(DrawingContext dc)
    {
        var size = Math.Min(ActualWidth, ActualHeight);
        if (size <= Thickness) return;
        var r = (size - Thickness) / 2;
        var c = new Point(ActualWidth / 2, ActualHeight / 2);
        if (Track is not null) dc.DrawEllipse(null, new Pen(Track, Thickness), c, r, r);
        var share = Waiting ? 0.22 : Math.Min(1, Progress);
        var pen = new Pen(Stroke, Thickness) { StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round };
        if (share >= 0.999) { dc.DrawEllipse(null, pen, c, r, r); return; }
        var end = -Math.PI / 2 + 2 * Math.PI * share;
        var geo = new StreamGeometry();
        using (var g = geo.Open())
        {
            g.BeginFigure(new Point(c.X, c.Y - r), false, false);
            g.ArcTo(new Point(c.X + r * Math.Cos(end), c.Y + r * Math.Sin(end)), new Size(r, r), 0, share > 0.5,
                    SweepDirection.Clockwise, true, false);
        }
        geo.Freeze();
        dc.DrawGeometry(null, pen, geo);
    }
}

/// <summary>A thin progress bar: the share done, filled from the left with rounded ends.</summary>
public sealed class Bar : FrameworkElement
{
    public static readonly DependencyProperty ProgressProperty = DependencyProperty.Register(
        nameof(Progress), typeof(double), typeof(Bar), new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty FillProperty = DependencyProperty.Register(
        nameof(Fill), typeof(Brush), typeof(Bar), new FrameworkPropertyMetadata(Brushes.DodgerBlue, FrameworkPropertyMetadataOptions.AffectsRender));
    public double Progress { get => (double)GetValue(ProgressProperty); set => SetValue(ProgressProperty, value); }
    public Brush Fill { get => (Brush)GetValue(FillProperty); set => SetValue(FillProperty, value); }

    protected override void OnRender(DrawingContext dc)
    {
        var w = ActualWidth * Math.Clamp(Progress, 0, 1);
        if (w <= 0) return;
        var r = ActualHeight / 2;
        dc.DrawRoundedRectangle(Fill, null, new Rect(0, 0, Math.Max(w, ActualHeight), ActualHeight), r, r);
    }
}

/// <summary>
/// The AirDrop mark: three concentric arcs open at the bottom around a dot,
/// drawn to any size in one brush (the title bars and the tray menu).
/// </summary>
public sealed class AirDropMark : FrameworkElement
{
    public static readonly DependencyProperty BrushProperty = DependencyProperty.Register(
        nameof(Brush), typeof(Brush), typeof(AirDropMark), new FrameworkPropertyMetadata(Brushes.DodgerBlue, FrameworkPropertyMetadataOptions.AffectsRender));
    public Brush Brush { get => (Brush)GetValue(BrushProperty); set => SetValue(BrushProperty, value); }

    protected override void OnRender(DrawingContext dc)
    {
        var size = Math.Min(ActualWidth, ActualHeight);
        if (size <= 0) return;
        var k = size / 20; // drawn on a 20 x 20 grid
        var c = new Point(ActualWidth / 2, ActualHeight / 2);
        var pen = new Pen(Brush, 1.6 * k) { StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round };
        foreach (var r in new[] { 8.2, 5.4 })
        {
            // From 120 degrees round the top to 60 degrees: a gap of 60 degrees at the bottom.
            var radius = r * k;
            var from = new Point(c.X + radius * Math.Cos(Math.PI * 2 / 3), c.Y + radius * Math.Sin(Math.PI * 2 / 3));
            var to = new Point(c.X + radius * Math.Cos(Math.PI / 3), c.Y + radius * Math.Sin(Math.PI / 3));
            var arc = new StreamGeometry();
            using (var g = arc.Open())
            {
                g.BeginFigure(from, false, false);
                g.ArcTo(to, new Size(radius, radius), 0, true, SweepDirection.Clockwise, true, false);
            }
            arc.Freeze();
            dc.DrawGeometry(null, pen, arc);
        }
        dc.DrawEllipse(Brush, null, c, 2.4 * k, 2.4 * k);
    }
}

/// <summary>True when the value is not null (a request is open).</summary>
public sealed class IsSet : System.Windows.Data.IValueConverter
{
    public static readonly IsSet Instance = new();
    public object? Convert(object? value, Type targetType, object? parameter, System.Globalization.CultureInfo culture) => value is not null;
    public object? ConvertBack(object? value, Type targetType, object? parameter, System.Globalization.CultureInfo culture) =>
        throw new NotSupportedException();
}

/// <summary>Windows' "Animation effects" setting: off means no looping motion.</summary>
public static class Motion
{
    /// <summary>Set by --snapshot: the build machine has animations off, and the pictures should show the ripple.</summary>
    public static bool ForceOn { get; set; }
    public static bool Reduced => !ForceOn && !SystemParameters.ClientAreaAnimation;
}
