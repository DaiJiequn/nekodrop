// SPDX-License-Identifier: GPL-2.0-only
using System.Collections.Specialized;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace NekoDrop;

public partial class MainWindow : Window
{
    static App A => App.Instance;

    public MainWindow()
    {
        InitializeComponent();
        DataContext = A.Model;
        SourceInitialized += (_, _) =>
        {
            if (Native.ApplyMica(this)) Background = Brushes.Transparent;
        };
        A.Model.Devices.CollectionChanged += Refresh;
        A.Model.Files.CollectionChanged += Refresh;
        Drop += OnDrop;
        DragOver += (_, e) => { e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None; e.Handled = true; };
        Closed += (_, _) =>
        {
            A.Model.Devices.CollectionChanged -= Refresh;
            A.Model.Files.CollectionChanged -= Refresh;
            A.WindowClosed();
        };
        Refresh(null, null);
        FolderText.Text = A.Settings.SaveFolder;
    }

    void ChangeFolder_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFolderDialog { Title = L.T("pick.folder"), InitialDirectory = A.Settings.SaveFolder };
        if (dlg.ShowDialog(this) != true) return;
        A.Settings.SaveFolder = dlg.FolderName;
        A.Settings.Save();
        A.Engine.Apply(A.Settings);
        FolderText.Text = dlg.FolderName;
    }

    public void FolderChanged() => FolderText.Text = A.Settings.SaveFolder;

    void Refresh(object? s, NotifyCollectionChangedEventArgs? e)
    {
        NoDevices.Visibility = A.Model.Devices.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        DropHint.Visibility = A.Model.Files.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    void OnDrop(object sender, DragEventArgs e)
    {
        if (e.Data.GetData(DataFormats.FileDrop) is string[] paths)
            foreach (var p in paths) if (!A.Model.Files.Contains(p)) A.Model.Files.Add(p);
    }

    void Device_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as Button)?.Tag is not Device d) return;
        if (A.Model.Files.Count == 0) { Add_Click(sender, e); if (A.Model.Files.Count == 0) return; }
        A.SendTo(d);
    }

    void Add_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog { Multiselect = true, Title = L.T("pick.files") };
        if (dlg.ShowDialog(this) == true)
            foreach (var f in dlg.FileNames) if (!A.Model.Files.Contains(f)) A.Model.Files.Add(f);
    }

    void Clear_Click(object sender, RoutedEventArgs e) => A.Model.Files.Clear();
    void Accept_Click(object sender, RoutedEventArgs e) => A.Answer((string)((Button)sender).Tag, true);
    void Decline_Click(object sender, RoutedEventArgs e) => A.Answer((string)((Button)sender).Tag, false);
    void Settings_Click(object sender, RoutedEventArgs e) => new SettingsWindow { Owner = this }.Show();
    void Wifi_Click(object sender, RoutedEventArgs e) => A.ShowWifi();
    void Report_Click(object sender, RoutedEventArgs e) => App.RunReport();

    void OpenFolder_Click(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(A.Settings.SaveFolder);
        Process.Start("explorer.exe", $"\"{A.Settings.SaveFolder}\"");
    }

    void Received_Click(object sender, MouseButtonEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is string path && File.Exists(path))
            Process.Start("explorer.exe", $"/select,\"{path}\"");
    }
}
