using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using Windows.Foundation;
using Windows.Foundation.Collections;
using GamEngine.Setup.Core;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace Installer
{
    /// <summary>
    /// An empty window that can be used on its own or navigated to within a Frame.
    /// </summary>
    public sealed partial class MainWindow : Window
    {
        private readonly bool _uninstall;

        public MainWindow(bool uninstall)
        {
            InitializeComponent();
            _uninstall = uninstall;
            InstallPath.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "GamEngine");
            if (uninstall)
            {
                Heading.Text = "Uninstall GamEngine";
                InstallPath.Text = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
                InstallPath.IsReadOnly = true;
                DesktopShortcut.Visibility = Visibility.Collapsed;
                StartMenuShortcut.Visibility = Visibility.Collapsed;
                InstallButton.Content = "Uninstall";
            }
        }

        private async void InstallButton_Click(object sender, RoutedEventArgs e)
        {
            InstallButton.IsEnabled = false;
            try
            {
                if (_uninstall)
                {
                    SetupIntegration.StartUninstall(InstallPath.Text);
                    Close();
                    return;
                }
                using var manifestStream = typeof(MainWindow).Assembly.GetManifestResourceStream("GamEngine.Manifest.json")
                    ?? throw new InvalidOperationException("This setup file does not contain a GamEngine package. Build it with the release script.");
                var manifest = PackageInstaller.ReadManifest(manifestStream);
                var root = InstallPath.Text.Trim();
                var desktop = DesktopShortcut.IsChecked == true;
                var startMenu = StartMenuShortcut.IsChecked == true;
                var progress = new Progress<InstallationProgress>(state =>
                {
                    InstallProgress.Value = state.Percent;
                    StatusText.Text = state.Message;
                });
                await Task.Run(() =>
                {
                    using var archive = typeof(MainWindow).Assembly.GetManifestResourceStream("GamEngine.Editor.zip")
                        ?? throw new InvalidOperationException("The setup file does not contain the editor archive.");
                    PackageInstaller.Install(archive, manifest, root, progress);
                    SetupIntegration.Register(root, manifest.Version, desktop, startMenu);
                });
                StatusText.Text = $"GamEngine {manifest.Version} is ready.";
                InstallButton.Content = "Done";
            }
            catch (Exception error)
            {
                StatusText.Text = "Error: " + error.Message;
                InstallButton.IsEnabled = true;
            }
        }
    }
}
