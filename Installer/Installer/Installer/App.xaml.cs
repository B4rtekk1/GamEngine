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
using Microsoft.UI.Xaml.Shapes;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Activation;
using Windows.Foundation;
using Windows.Foundation.Collections;
using GamEngine.Setup.Core;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace Installer
{
    /// <summary>
    /// Provides application-specific behavior to supplement the default Application class.
    /// </summary>
    public partial class App : Application
    {
        private Window? _window;

        /// <summary>
        /// Initializes the singleton application object.  This is the first line of authored code
        /// executed, and as such is the logical equivalent of main() or WinMain().
        /// </summary>
        public App()
        {
            InitializeComponent();
        }

        /// <summary>
        /// Invoked when the application is launched.
        /// </summary>
        /// <param name="args">Details about the launch request and process.</param>
        protected override void OnLaunched(Microsoft.UI.Xaml.LaunchActivatedEventArgs args)
        {
            var command = Environment.GetCommandLineArgs();
            if (command.Length >= 2 && command[1] == "--rollback")
            {
                try
                {
                    PackageInstaller.Rollback(AppContext.BaseDirectory);
                    Environment.Exit(0);
                }
                catch (Exception error)
                {
                    File.WriteAllText(System.IO.Path.Combine(System.IO.Path.GetTempPath(), "GamEngineRollback.log"), error.ToString());
                    Environment.Exit(1);
                }
            }
            if (command.Length >= 4 && command[1] == "--uninstall-worker")
            {
                try
                {
                    SetupIntegration.UninstallWorker(command[2], int.Parse(command[3]));
                    Environment.Exit(0);
                }
                catch (Exception error)
                {
                    File.WriteAllText(System.IO.Path.Combine(System.IO.Path.GetTempPath(), "GamEngineUninstall.log"), error.ToString());
                    Environment.Exit(1);
                }
            }
            _window = new MainWindow(command.Length >= 2 && command[1] == "--uninstall");
            _window.Activate();
        }
    }
}
