using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace GamEngine.Setup.Core;

public static class PackageInstaller
{
    public const string MarkerFile = "gamengine-install.json";
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true, WriteIndented = true };
    private static readonly Regex VersionPattern = new(@"^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$", RegexOptions.CultureInvariant);

    public static PackageManifest ReadManifest(Stream source)
    {
        var manifest = JsonSerializer.Deserialize<PackageManifest>(source, JsonOptions)
            ?? throw new InvalidDataException("The package manifest is empty.");
        ValidateManifest(manifest);
        return manifest;
    }

    public static void ValidateManifest(PackageManifest manifest)
    {
        if (manifest.Files is null) throw new InvalidDataException("The manifest has no file list.");
        if (!VersionPattern.IsMatch(manifest.Version))
            throw new InvalidDataException("The manifest contains an invalid version number.");
        if (manifest.Files.Count == 0)
            throw new InvalidDataException("The manifest contains no files.");
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var file in manifest.Files)
        {
            var path = NormalizeRelativePath(file.Path);
            if (!paths.Add(path) || file.Size < 0 || !Regex.IsMatch(file.Sha256, "^[a-fA-F0-9]{64}$"))
                throw new InvalidDataException($"Invalid manifest entry: {file.Path}");
        }
        foreach (var required in new[] { "Editor.exe", "Engine.dll", "DearImGui.dll", "SDL3.dll", "Tools/Slang/slangc.exe" })
            if (!paths.Contains(required)) throw new InvalidDataException($"The package is missing {required}.");
        if (!paths.Any(p => p.StartsWith("shaders/", StringComparison.OrdinalIgnoreCase)) ||
            !paths.Any(p => p.StartsWith("SDK/", StringComparison.OrdinalIgnoreCase)) ||
            !paths.Any(p => p.StartsWith("Runtime/Win64/Release/", StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException("The package is missing shaders, the SDK, or the Player template.");
    }

    public static string NormalizeRelativePath(string path)
    {
        var normalized = path.Replace('\\', '/');
        if (string.IsNullOrWhiteSpace(normalized) || normalized.StartsWith('/') || normalized.Contains(':') ||
            normalized.Split('/').Any(part => part is "" or "." or ".."))
            throw new InvalidDataException($"Invalid path in package: {path}");
        return normalized;
    }

    public static string? ReadCurrentVersion(string root)
        => ReadCurrentState(root)?.Version;

    public static CurrentVersion? ReadCurrentState(string root)
    {
        var path = System.IO.Path.Combine(root, "current.json");
        if (!File.Exists(path)) return null;
        var state = JsonSerializer.Deserialize<CurrentVersion>(File.ReadAllText(path), JsonOptions);
        if (state is null || !VersionPattern.IsMatch(state.Version) ||
            (state.PreviousVersion is not null && !VersionPattern.IsMatch(state.PreviousVersion)))
            throw new InvalidDataException("Invalid current.json.");
        return state;
    }

    public static void Rollback(string root)
    {
        var state = ReadCurrentState(root) ?? throw new InvalidOperationException("No active version was found.");
        var previous = state.PreviousVersion ?? throw new InvalidOperationException("No previous version was found.");
        if (!Directory.Exists(System.IO.Path.Combine(root, "Versions", previous)))
            throw new InvalidOperationException("The previous version is unavailable.");
        WriteJsonAtomically(System.IO.Path.Combine(root, "current.json"),
            new CurrentVersion { Version = previous, PreviousVersion = state.Version });
    }

    public static void Install(Stream archiveStream, PackageManifest manifest, string root, IProgress<InstallationProgress>? progress = null)
    {
        ValidateManifest(manifest);
        root = System.IO.Path.GetFullPath(root);
        ValidateDestination(root);
        var requiredBytes = manifest.Files.Sum(file => file.Size);
        var drive = new DriveInfo(System.IO.Path.GetPathRoot(root)!);
        if (drive.AvailableFreeSpace < requiredBytes + Math.Min(requiredBytes, 512L * 1024 * 1024))
            throw new IOException("There is not enough free space to install GamEngine.");
        var stage = System.IO.Path.Combine(root, $".staging-{Guid.NewGuid():N}");
        var target = System.IO.Path.Combine(root, "Versions", manifest.Version);
        var backup = System.IO.Path.Combine(root, "Versions", $".backup-{Guid.NewGuid():N}");
        var previousState = ReadCurrentState(root);
        var previous = previousState?.Version;
        var hadTarget = Directory.Exists(target);
        Directory.CreateDirectory(stage);
        try
        {
            var expected = manifest.Files.ToDictionary(f => NormalizeRelativePath(f.Path), StringComparer.OrdinalIgnoreCase);
            using (var archive = new ZipArchive(archiveStream, ZipArchiveMode.Read, leaveOpen: true))
            {
                var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                long total = manifest.Files.Sum(f => f.Size);
                long written = 0;
                foreach (var entry in archive.Entries)
                {
                    if (entry.FullName.EndsWith('/')) continue;
                    if (((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000)
                        throw new InvalidDataException("The archive contains an unsupported symbolic link.");
                    var relative = NormalizeRelativePath(entry.FullName);
                    if (!expected.TryGetValue(relative, out var file) || !seen.Add(relative) || entry.Length != file.Size)
                        throw new InvalidDataException($"Unexpected file in archive: {entry.FullName}");
                    var destination = System.IO.Path.Combine(stage, relative.Replace('/', System.IO.Path.DirectorySeparatorChar));
                    Directory.CreateDirectory(System.IO.Path.GetDirectoryName(destination)!);
                    using var source = entry.Open();
                    using var output = File.Create(destination);
                    using var hash = SHA256.Create();
                    var buffer = new byte[1024 * 1024];
                    int count;
                    while ((count = source.Read(buffer, 0, buffer.Length)) != 0)
                    {
                        output.Write(buffer, 0, count);
                        hash.TransformBlock(buffer, 0, count, null, 0);
                    }
                    hash.TransformFinalBlock([], 0, 0);
                    if (!Convert.ToHexString(hash.Hash!).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException($"SHA-256 mismatch: {relative}");
                    written += file.Size;
                    progress?.Report(new InstallationProgress(total == 0 ? 90 : (int)(written * 90 / total), relative));
                }
                if (seen.Count != expected.Count) throw new InvalidDataException("The archive is missing files listed in the manifest.");
            }
            Directory.CreateDirectory(System.IO.Path.Combine(root, "Versions"));
            if (hadTarget) Directory.Move(target, backup);
            Directory.Move(stage, target);
            WriteJsonAtomically(System.IO.Path.Combine(root, "current.json"), new CurrentVersion
            {
                Version = manifest.Version,
                PreviousVersion = previous == manifest.Version ? previousState?.PreviousVersion : previous
            });
            WriteJsonAtomically(System.IO.Path.Combine(root, MarkerFile), new CurrentVersion { Version = manifest.Version });
            if (hadTarget) Directory.Delete(backup, recursive: true);
            progress?.Report(new InstallationProgress(100, "The new version is active."));
        }
        catch
        {
            if (Directory.Exists(stage)) Directory.Delete(stage, recursive: true);
            if (!hadTarget && Directory.Exists(target) && previous != manifest.Version)
                Directory.Delete(target, recursive: true);
            if (Directory.Exists(backup))
            {
                if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
                Directory.Move(backup, target);
            }
            if (previous is not null && Directory.Exists(System.IO.Path.Combine(root, "Versions", previous)))
                WriteJsonAtomically(System.IO.Path.Combine(root, "current.json"), previousState!);
            throw;
        }
    }

    public static void ValidateDestination(string root)
    {
        var full = System.IO.Path.GetFullPath(root).TrimEnd(System.IO.Path.DirectorySeparatorChar);
        if (full.Equals(System.IO.Path.GetPathRoot(full)?.TrimEnd(System.IO.Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase) ||
            full.Equals(Environment.GetFolderPath(Environment.SpecialFolder.Windows), StringComparison.OrdinalIgnoreCase) ||
            full.Equals(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), StringComparison.OrdinalIgnoreCase))
            throw new IOException("Choose a dedicated folder for GamEngine.");
        if (Directory.Exists(full) && Directory.EnumerateFileSystemEntries(full).Any() &&
            !File.Exists(System.IO.Path.Combine(full, MarkerFile)))
            throw new IOException("The selected folder is not empty and does not contain an existing GamEngine installation.");
    }

    public static void WriteJsonAtomically<T>(string path, T value)
    {
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(value, JsonOptions));
        try { File.Move(temporary, path, overwrite: true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}
