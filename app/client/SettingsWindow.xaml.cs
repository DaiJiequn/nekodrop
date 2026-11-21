// SPDX-License-Identifier: GPL-2.0-only
using System.Windows;
using System.Windows.Media;

namespace NekoDrop;

public partial class SettingsWindow : Window
{
    static App A => App.Instance;

    public SettingsWindow()
    {
        InitializeComponent();
        SourceInitialized += (_, _) => { if (Native.ApplyMica(this)) Background = Brushes.Transparent; };
        var s = A.Settings;
        NameBox.Text = s.DeviceName;
        FolderBox.Text = s.SaveFolder;
        AskBox.IsChecked = s.Ask;
        StartupBox.IsChecked = s.StartWithWindows;
        LanguageBox.SelectedIndex = s.Language switch { "zh" => 1, "en" => 2, _ => 0 };
        foreach (var t in s.AlwaysAccept) TrustedList.Items.Add(t.Replace("|", " · "));
        var mode = (string?)A.Service.Last?["mode"] ?? "concurrent";
        ModeConcurrent.IsChecked = mode == "concurrent";
        ModeOnDemand.IsChecked = mode == "ondemand";
        ModeOff.IsChecked = mode == "off";
        FastSendBox.IsChecked = (bool?)A.Service.Last?["fastSend"] ?? false;
        AggregateBox.IsChecked = (bool?)A.Service.Last?["aggregate"] ?? false;
    }

    void Browse_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFolderDialog { Title = L.T("pick.folder"), InitialDirectory = FolderBox.Text };
        if (dlg.ShowDialog(this) == true) FolderBox.Text = dlg.FolderName;
    }

    void RemoveTrusted_Click(object sender, RoutedEventArgs e)
    {
        if (TrustedList.SelectedIndex >= 0) TrustedList.Items.RemoveAt(TrustedList.SelectedIndex);
    }

    void Save_Click(object sender, RoutedEventArgs e)
    {
        var s = A.Settings;
        var name = NameBox.Text.Trim();
        if (name.Length > 0) s.DeviceName = name;
        s.SaveFolder = FolderBox.Text;
        s.Ask = AskBox.IsChecked == true;
        s.StartWithWindows = StartupBox.IsChecked == true;
        var language = (LanguageBox.SelectedItem as System.Windows.Controls.ComboBoxItem)?.Tag as string ?? "auto";
        var languageChanged = language != s.Language;
        s.Language = language;
        s.AlwaysAccept = TrustedList.Items.Cast<string>().Select(x => x.Replace(" · ", "|")).ToList();
        s.Save();
        try { Shell.Register(s.StartWithWindows); } catch (Exception) { }
        A.Engine.Apply(s);
        var mode = ModeConcurrent.IsChecked == true ? "concurrent" : ModeOnDemand.IsChecked == true ? "ondemand" : "off";
        if (mode != (string?)A.Service.Last?["mode"]) A.Service.SetMode(mode);
        var fast = FastSendBox.IsChecked == true;
        if (fast != ((bool?)A.Service.Last?["fastSend"] ?? false)) A.Service.SetFastSend(fast);
        var aggregate = AggregateBox.IsChecked == true;
        if (aggregate != ((bool?)A.Service.Last?["aggregate"] ?? false)) A.Service.SetAggregate(aggregate);
        A.Window?.FolderChanged();
        if (languageChanged) A.ApplyLanguage();
        Close();
    }

    void Cancel_Click(object sender, RoutedEventArgs e) => Close();
}
