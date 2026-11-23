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
    public const double Node = 56;
    /// <summary>A device's whole box: icon, name and status below it.</summary>
    public const double NodeWidth = 116, NodeHeight = Node + 44;
    /// <summary>"This PC" in the middle.</summary>
    public const double Hub = 60;
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
        return new RadarGeometry(center, orbit, orbit * 0.52, orbit * 1.45);
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
            // The icon is at the top of the box: its center is half an icon down.
            InternalChildren[i].Arrange(new Rect(p.X - RadarGeometry.NodeWidth / 2, p.Y - RadarGeometry.Node / 2,
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

/// <summary>Windows' "Animation effects" setting: off means no looping motion.</summary>
public static class Motion
{
    /// <summary>Set by --snapshot: the build machine has animations off, and the pictures should show the ripple.</summary>
    public static bool ForceOn { get; set; }
    public static bool Reduced => !ForceOn && !SystemParameters.ClientAreaAnimation;
}
