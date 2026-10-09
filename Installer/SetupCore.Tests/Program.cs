using System.IO.Compression;
using System.Security.Cryptography;
using GamEngine.Setup.Core;

var root = Path.Combine(Path.GetTempPath(), "GamEngineSetupTests-" + Guid.NewGuid().ToString("N"));
try
{
    var files = new Dictionary<string, byte[]>
    {
        ["Editor.exe"] = [1], ["Engine.dll"] = [2], ["DearImGui.dll"] = [3], ["SDL3.dll"] = [4],
        ["Tools/Slang/slangc.exe"] = [5], ["shaders/test.spv"] = [6], ["SDK/Include/test.h"] = [7],
        ["Runtime/Win64/Release/Player.exe"] = [8]
    };
    (PackageManifest manifest, MemoryStream zip) Build(string version, bool corrupt = false)
    {
        var manifest = new PackageManifest { Version = version };
        var zip = new MemoryStream();
        using (var archive = new ZipArchive(zip, ZipArchiveMode.Create, leaveOpen: true))
            foreach (var (name, data) in files)
            {
                manifest.Files.Add(new PackageFile
                {
                    Path = name, Size = data.Length, Sha256 = Convert.ToHexString(SHA256.HashData(data))
                });
                using var stream = archive.CreateEntry(name).Open();
                stream.Write(corrupt && name == "Editor.exe" ? new byte[] { 99 } : data);
            }
        zip.Position = 0;
        return (manifest, zip);
    }

    var (first, firstZip) = Build("0.1.0");
    using (firstZip) PackageInstaller.Install(firstZip, first, root);
    if (PackageInstaller.ReadCurrentVersion(root) != "0.1.0") throw new Exception("First install failed");
    var (second, secondZip) = Build("0.2.0");
    using (secondZip) PackageInstaller.Install(secondZip, second, root);
    if (PackageInstaller.ReadCurrentVersion(root) != "0.2.0" ||
        !File.Exists(Path.Combine(root, "Versions", "0.1.0", "Editor.exe")))
        throw new Exception("Update did not retain the previous version");
    var (bad, badZip) = Build("0.3.0", corrupt: true);
    try { using (badZip) PackageInstaller.Install(badZip, bad, root); throw new Exception("Corrupt package accepted"); }
    catch (InvalidDataException) { }
    if (PackageInstaller.ReadCurrentVersion(root) != "0.2.0" ||
        Directory.Exists(Path.Combine(root, "Versions", "0.3.0")))
        throw new Exception("Rollback after failed update did not work");
    PackageInstaller.Rollback(root);
    if (PackageInstaller.ReadCurrentVersion(root) != "0.1.0")
        throw new Exception("Explicit rollback did not work");
    try { PackageInstaller.NormalizeRelativePath("../escape.exe"); throw new Exception("Traversal accepted"); }
    catch (InvalidDataException) { }
    Console.WriteLine("Install, update, rollback and path validation passed.");

    if (args.Length == 2)
    {
        var releaseRoot = Path.Combine(Path.GetTempPath(), "GamEngineReleaseTest-" + Guid.NewGuid().ToString("N"));
        try
        {
            using var manifestFile = File.OpenRead(args[0]);
            var releaseManifest = PackageInstaller.ReadManifest(manifestFile);
            using var archiveFile = File.OpenRead(args[1]);
            PackageInstaller.Install(archiveFile, releaseManifest, releaseRoot);
            if (PackageInstaller.ReadCurrentVersion(releaseRoot) != releaseManifest.Version)
                throw new Exception("Release activation failed");
            Console.WriteLine($"Release package {releaseManifest.Version} verified and installed in a temporary folder.");
        }
        finally { if (Directory.Exists(releaseRoot)) Directory.Delete(releaseRoot, recursive: true); }
    }
}
finally { if (Directory.Exists(root)) Directory.Delete(root, recursive: true); }
