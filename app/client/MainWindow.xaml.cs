// SPDX-License-Identifier: GPL-2.0-only
using System.Collections.Specialized;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Shape = System.Windows.Shapes.Shape;

namespace NekoDrop;

/// <summary>
/// The main window: its own title bar (status pill with the quick controls,
/// Wi-Fi, settings), a newer-version bar, someone asking to send, nearby
/// devices (radar or list), the files to send, what came in lately, and where
/// it is saved.
/// </summary>
public partial class MainWindow : Window
{
    static App A => App.Instance;

    public MainWindow()
    {
        InitializeComponent();
        Native.Chrome(this);
        DataContext = A.Model;
        ReceivedList.ItemsSource = A.Model.ReceivedFiles;
        A.Model.Files.CollectionChanged += OnFiles;
        A.Model.ReceivedFiles.CollectionChanged += OnReceived;
        A.Model.PropertyChanged += OnModel;
        Drop += OnDrop;
        DragOver += (_, e) => { e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None; e.Handled = true; };
        Closed += (_, _) =>
        {
            A.Model.Files.CollectionChanged -= OnFiles;
            A.Model.ReceivedFiles.CollectionChanged -= OnReceived;
            A.Model.PropertyChanged -= OnModel;
            A.WindowClosed();
        };
        FolderChanged();
        OnFiles(null, null);
        OnReceived(null, null);
        ShowState();
    }

    public void FolderChanged() => FolderText.Text = L.T("main.saveTo") + " " + A.Settings.SaveFolder;

    void OnModel(object? s, PropertyChangedEventArgs e) => ShowState();

    /// <summary>The status pill, the Wi-Fi button and the update bar.</summary>
    void ShowState()
    {
        var m = A.Model;
        var (label, dot) = Status(m.Air);
        StatusText.Text = label;
        StatusDot.SetResourceReference(Shape.FillProperty, dot);
        StatusPill.ToolTip = m.Air == "ready" ? L.F("status.on", m.Channel) : label;
        WifiText.Text = m.Mode == "concurrent" && m.Ssid != "" ? m.Ssid : "Wi-Fi";
        UpdateBar.Visibility = m.Update == "" ? Visibility.Collapsed : Visibility.Visible;
        UpdateText.Text = L.F(m.UpdatePreview ? "update.barPreview" : "update.bar", m.Update);
    }

    /// <summary>The words and dot colour of a state (the design's status pill).</summary>
    public static (string Label, string Dot) Status(string air) => air switch
    {
        "ready" => (L.T("pill.ready"), "Success"),
        "starting" => (L.T("status.starting"), "Warn"),
        "standby" => (L.T("pill.standby"), "Text3"),
        "off" => (L.T("pill.off"), "Text3"),
        "error" => (L.T("status.error"), "Error"),
        "no_adapter" => (L.T("radar.noCard"), "Warn"),
        _ => (L.T("status.noService"), "Error"),
    };

    // ------------------------------------------------------- quick controls

    void StatusPill_Checked(object sender, RoutedEventArgs e)
    {
        AllowToggle.IsChecked = A.Model.Allow;
        ModeList.Children.Clear();
        foreach (var (key, text) in new[] { ("concurrent", L.T("mode.concurrent.short")), ("ondemand", L.T("mode.ondemand.short")), ("off", L.T("mode.off.short")) })
            ModeList.Children.Add(ModeChoice(key, text, A.Model.Mode == key));
        ChannelTip.Text = A.Model.Air == "ready" ? L.F("status.on", A.Model.Channel) : Status(A.Model.Air).Label;
        StatusPopup.IsOpen = true;
    }

    Button ModeChoice(string key, string text, bool chosen)
    {
        var dot = new Grid { Width = 12, Height = 12, Margin = new Thickness(0, 0, 8, 0), VerticalAlignment = VerticalAlignment.Center };
        var ring = new System.Windows.Shapes.Ellipse { StrokeThickness = 1.5 };
        ring.SetResourceReference(Shape.StrokeProperty, chosen ? "Accent" : "Text3");
        dot.Children.Add(ring);
        if (chosen)
        {
            var fill = new System.Windows.Shapes.Ellipse { Width = 6, Height = 6 };
            fill.SetResourceReference(Shape.FillProperty, "Accent");
            dot.Children.Add(fill);
        }
        var label = new TextBlock { Text = text, FontSize = 12, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
        if (chosen) { label.FontWeight = FontWeights.SemiBold; label.SetResourceReference(TextBlock.ForegroundProperty, "Accent"); }
        var row = new StackPanel { Orientation = Orientation.Horizontal };
        row.Children.Add(dot);
        row.Children.Add(label);
        var b = new Button
        {
            Style = (Style)FindResource("TitleButton"),
            Width = double.NaN,
            Height = double.NaN,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Left,
            Padding = new Thickness(8, 6, 8, 6),
            Content = row,
        };
        if (chosen) b.SetResourceReference(BackgroundProperty, "AccentSubtle");
        b.Click += (_, _) => { A.Service.SetMode(key); StatusPopup.IsOpen = false; };
        return b;
    }

    void StatusPopup_Closed(object? sender, EventArgs e) => StatusPill.IsChecked = false;
    void Allow_Click(object sender, RoutedEventArgs e) => A.SetDiscoverable(AllowToggle.IsChecked == true);

    // ----------------------------------------------------------- the files

    void OnFiles(object? s, NotifyCollectionChangedEventArgs? e)
    {
        var n = A.Model.Files.Count;
        FilesRow.Visibility = n == 0 ? Visibility.Collapsed : Visibility.Visible;
        ClearButton.Visibility = n == 0 ? Visibility.Collapsed : Visibility.Visible;
        FilesCount.Text = L.F("main.count", n);
        FilesOutline.StrokeDashArray = n == 0 ? new DoubleCollection { 4, 3 } : null;
    }

    void OnReceived(object? s, NotifyCollectionChangedEventArgs? e)
    {
        NothingReceived.Visibility = A.Model.ReceivedFiles.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        // The latest three, as in the design.
        ReceivedList.ItemsSource = A.Model.ReceivedFiles.Take(3).ToList();
    }

    void OnDrop(object sender, DragEventArgs e)
    {
        if (e.Data.GetData(DataFormats.FileDrop) is string[] paths)
            foreach (var p in paths) if (!A.Model.Files.Contains(p)) A.Model.Files.Add(p);
    }

    void Add_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog { Multiselect = true, Title = L.T("pick.files") };
        if (dlg.ShowDialog(this) == true)
            foreach (var f in dlg.FileNames) if (!A.Model.Files.Contains(f)) A.Model.Files.Add(f);
    }

    void Clear_Click(object sender, RoutedEventArgs e) => A.Model.Files.Clear();

    void RemoveFile_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is string f) A.Model.Files.Remove(f);
    }

    // ------------------------------------------------------------ the rest

    void Accept_Click(object sender, RoutedEventArgs e) => A.Answer((string)((Button)sender).Tag, true);
    void Decline_Click(object sender, RoutedEventArgs e) => A.Answer((string)((Button)sender).Tag, false);
    void Settings_Click(object sender, RoutedEventArgs e) => A.ShowSettings("general");
    void Wifi_Click(object sender, RoutedEventArgs e) => A.ShowWifi();
    void Report_Click(object sender, RoutedEventArgs e) { StatusPopup.IsOpen = false; App.RunReport(); }
    void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    void Close_Click(object sender, RoutedEventArgs e) => Close();

    async void UpdateNow_Click(object sender, RoutedEventArgs e)
    {
        UpdateBar.IsEnabled = false;
        var why = await A.InstallUpdate();
        UpdateBar.IsEnabled = true;
        if (why != "") UpdateText.Text = L.T("update.failed") + ": " + why;
    }

    void UpdateLater_Click(object sender, RoutedEventArgs e)
    {
        A.Settings.SkipVersion = A.Model.Update;
        A.Settings.Save();
        A.Model.Update = "";
    }

    void ChangeFolder_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFolderDialog { Title = L.T("pick.folder"), InitialDirectory = A.Settings.SaveFolder };
        if (dlg.ShowDialog(this) != true) return;
        A.Settings.SaveFolder = dlg.FolderName;
        A.Settings.Save();
        A.Engine.Apply(A.Settings);
        FolderChanged();
    }

    void OpenFolder_Click(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(A.Settings.SaveFolder);
        Process.Start("explorer.exe", $"\"{A.Settings.SaveFolder}\"");
    }

    void Open_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is string path && File.Exists(path))
            Process.Start("explorer.exe", $"/select,\"{path}\"");
    }
}

/// <summary>A path's last part, for the file chips.</summary>
public sealed class FileName : System.Windows.Data.IValueConverter
{
    public static readonly FileName Instance = new();
    public object? Convert(object? value, Type targetType, object? parameter, System.Globalization.CultureInfo culture) =>
        value is string s ? Path.GetFileName(s.TrimEnd('\\')) : "";
    public object? ConvertBack(object? value, Type targetType, object? parameter, System.Globalization.CultureInfo culture) =>
        throw new NotSupportedException();
}
