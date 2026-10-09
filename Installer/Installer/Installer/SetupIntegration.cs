using System.Diagnostics;
using System.Runtime.InteropServices;
using GamEngine.Setup.Core;
using Microsoft.Win32;

namespace Installer;

internal static class SetupIntegration
{
    private const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\GamEngine";
    private const int MoveFileDelayUntilReboot = 0x4;

    public static void Register(string root, string version, bool desktop, bool startMenu)
    {
        root = Path.GetFullPath(root);
        var launcher = Path.Combine(root, "GamEngineLauncher.exe");
        using (var source = typeof(SetupIntegration).Assembly.GetManifestResourceStream("GamEngine.Launcher.exe")
            ?? throw new InvalidOperationException("The setup file does not contain the launcher."))
        using (var output = File.Create(launcher)) source.CopyTo(output);

        var sourceSetup = Environment.ProcessPath ?? throw new InvalidOperationException("Cannot determine the setup file path.");
        var installedSetup = Path.Combine(root, "GamEngineSetup.exe");
        if (!Path.GetFullPath(sourceSetup).Equals(installedSetup, StringComparison.OrdinalIgnoreCase))
            File.Copy(sourceSetup, installedSetup, overwrite: true);

        var desktopLink = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "GamEngine.lnk");
        var menuDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.StartMenu), "Programs", "GamEngine");
        var menuLink = Path.Combine(menuDirectory, "GamEngine.lnk");
        if (desktop) CreateShortcut(desktopLink, launcher, root);
        else if (File.Exists(desktopLink)) File.Delete(desktopLink);
        if (startMenu) CreateShortcut(menuLink, launcher, root);
        else if (File.Exists(menuLink)) File.Delete(menuLink);

        using var key = Registry.CurrentUser.CreateSubKey(UninstallKey);
        key.SetValue("DisplayName", "GamEngine");
        key.SetValue("DisplayVersion", version);
        key.SetValue("Publisher", "GamEngine");
        key.SetValue("InstallLocation", root);
        key.SetValue("DisplayIcon", launcher);
        key.SetValue("UninstallString", $"\"{installedSetup}\" --uninstall");
        key.SetValue("EstimatedSize", (int)Math.Min(int.MaxValue,
            Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories).Sum(file => new FileInfo(file).Length) / 1024), RegistryValueKind.DWord);
    }

    private static void CreateShortcut(string path, string target, string workingDirectory)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var shellType = Type.GetTypeFromProgID("WScript.Shell")
            ?? throw new InvalidOperationException("Cannot create a Windows shortcut.");
        dynamic shell = Activator.CreateInstance(shellType)!;
        dynamic link = shell.CreateShortcut(path);
        link.TargetPath = target;
        link.WorkingDirectory = workingDirectory;
        link.IconLocation = target;
        link.Save();
        Marshal.FinalReleaseComObject(link);
        Marshal.FinalReleaseComObject(shell);
    }

    public static void StartUninstall(string root)
    {
        root = Path.GetFullPath(root);
        if (!File.Exists(Path.Combine(root, PackageInstaller.MarkerFile)))
            throw new InvalidOperationException("No GamEngine installation was found in this folder.");
        var temporary = Path.Combine(Path.GetTempPath(), $"GamEngineUninstall-{Guid.NewGuid():N}.exe");
        File.Copy(Environment.ProcessPath!, temporary);
        var info = new ProcessStartInfo(temporary) { UseShellExecute = true };
        info.ArgumentList.Add("--uninstall-worker");
        info.ArgumentList.Add(root);
        info.ArgumentList.Add(Environment.ProcessId.ToString());
        Process.Start(info);
    }

    public static void UninstallWorker(string root, int parentProcessId)
    {
        try { Process.GetProcessById(parentProcessId).WaitForExit(30000); }
        catch (ArgumentException) { }
        root = Path.GetFullPath(root);
        if (!File.Exists(Path.Combine(root, PackageInstaller.MarkerFile))) return;
        PackageInstaller.ValidateDestination(root);
        var versions = Path.Combine(root, "Versions");
        if (Directory.Exists(versions)) Directory.Delete(versions, recursive: true);
        foreach (var name in new[] { "current.json", PackageInstaller.MarkerFile, "GamEngineLauncher.exe", "GamEngineSetup.exe" })
        {
            var path = Path.Combine(root, name);
            if (File.Exists(path)) File.Delete(path);
        }
        var desktopLink = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "GamEngine.lnk");
        var menuDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.StartMenu), "Programs", "GamEngine");
        var menuLink = Path.Combine(menuDirectory, "GamEngine.lnk");
        if (File.Exists(desktopLink)) File.Delete(desktopLink);
        if (File.Exists(menuLink)) File.Delete(menuLink);
        if (Directory.Exists(menuDirectory) && !Directory.EnumerateFileSystemEntries(menuDirectory).Any()) Directory.Delete(menuDirectory);
        if (!Directory.EnumerateFileSystemEntries(root).Any()) Directory.Delete(root);
        Registry.CurrentUser.DeleteSubKey(UninstallKey, throwOnMissingSubKey: false);
        if (Environment.ProcessPath is { } self) MoveFileEx(self, null, MoveFileDelayUntilReboot);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool MoveFileEx(string existingFileName, string? newFileName, int flags);
}
