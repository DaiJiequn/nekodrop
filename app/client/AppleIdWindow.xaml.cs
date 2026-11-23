// SPDX-License-Identifier: GPL-2.0-only
using System.Windows;
using System.Windows.Media;

namespace NekoDrop;

/// <summary>
/// Signing in to an Apple ID for Contacts Only: e-mail and password, then the
/// verification code if the account asks for one. appleid.exe does the work
/// (AppleId.SignIn); the password goes to it once and is never stored.
/// </summary>
public partial class AppleIdWindow : Window
{
    TaskCompletionSource<string?>? _code;
    bool _busy;

    public AppleIdWindow()
    {
        InitializeComponent();
        Native.Chrome(this);
        Loaded += (_, _) => EmailBox.Focus();
    }

    void Say(string text, bool error = false)
    {
        StatusText.Text = text;
        StatusText.SetResourceReference(ForegroundProperty, error ? "Error" : "Text2");
    }

    async void Go_Click(object sender, RoutedEventArgs e)
    {
        if (_code is not null)
        {
            var typed = CodeBox.Text.Trim();
            if (typed.Length < 6) { Say(L.T("aid.codeShort"), true); return; }
            GoButton.IsEnabled = false;
            _code.TrySetResult(typed);
            _code = null;
            return;
        }
        if (_busy) return;
        var email = EmailBox.Text.Trim();
        var password = PasswordBox.Password;
        if (email.Length == 0 || password.Length == 0) { Say(L.T("aid.fill"), true); return; }
        _busy = true;
        GoButton.IsEnabled = false;
        Credentials.IsEnabled = false;
        Say(L.T("aid.working"));
        var failure = await AppleId.SignIn(email, password,
            ev => Dispatcher.Invoke(() =>
            {
                if ((string?)ev["ev"] == "status" && (string?)ev["msg"] is { } msg) Say(msg);
            }),
            sent => Dispatcher.Invoke(() =>
            {
                _code = new TaskCompletionSource<string?>();
                Credentials.Visibility = Visibility.Collapsed;
                CodePanel.Visibility = Visibility.Visible;
                CodeHint.Text = L.T(sent ? "aid.codeSent" : "aid.codeSms");
                GoButton.Content = L.T("aid.verify");
                GoButton.IsEnabled = true;
                CodeBox.Focus();
                Say("");
                return _code.Task;
            }));
        _busy = false;
        if (failure is null)
        {
            DialogResult = true;
            return;
        }
        Say(failure, true);
        Credentials.IsEnabled = true;
        Credentials.Visibility = Visibility.Visible;
        CodePanel.Visibility = Visibility.Collapsed;
        GoButton.Content = L.T("aid.signIn");
        GoButton.IsEnabled = true;
    }

    void Cancel_Click(object sender, RoutedEventArgs e)
    {
        _code?.TrySetResult(null);
        if (!_busy) Close();
    }
}
