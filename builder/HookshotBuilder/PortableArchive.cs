using System.IO;
using System.IO.Compression;

namespace HookshotBuilder;

/// ZIP fallback shares the native setup's version registry; a failed extraction never replaces an installed version.
static class PortableArchive
{
    internal static string Install(string zip, string home, string tag)
    {
        if (tag.Length == 0 || tag.Length > 127 || tag[0] == '.' ||
            tag.Any(c => !(char.IsAsciiLetterOrDigit(c) || c is '.' or '_' or '+' or '-')))
            throw new InvalidDataException("The release tag is not a setup version name.");
        var resolved = BuilderHome.Resolve(home, home, home, portableRequested: true);
        var versions = Path.Combine(resolved.Root, "versions");
        Directory.CreateDirectory(versions);
        var destination = Path.Combine(versions, tag);
        var target = Path.Combine(destination, "HookshotBuilder");
        var staging = Path.Combine(versions, ".unpack-" + Guid.NewGuid().ToString("N"));
        try
        {
            // Never overwrite an existing (possibly running) installation, even when it is incomplete.
            if (Directory.Exists(destination)) Validate(target, tag);
            else
            {
                ZipFile.ExtractToDirectory(zip, staging);
                var nested = Path.Combine(staging, "HookshotBuilder");
                var payload = Directory.Exists(nested) ? nested : staging;
                Validate(payload, tag);
                // Publish the complete version directory before changing the current pointer.
                if (payload == staging)
                {
                    var container = Path.Combine(staging, ".container");
                    Directory.CreateDirectory(container);
                    foreach (var entry in Directory.EnumerateFileSystemEntries(staging).ToArray())
                        if (entry != container)
                        {
                            var to = Path.Combine(container, Path.GetFileName(entry));
                            if (Directory.Exists(entry)) Directory.Move(entry, to); else File.Move(entry, to);
                        }
                    Directory.Move(container, Path.Combine(staging, "HookshotBuilder"));
                }
                Directory.Move(staging, destination);
            }
            var current = Path.Combine(versions, "current.txt");
            var previous = File.Exists(current) ? File.ReadAllText(current).Trim() : "";
            if (previous != tag)
            {
                if (previous.Length != 0) WritePointer(Path.Combine(versions, "previous.txt"), previous);
                WritePointer(current, tag);
            }
            return target;
        }
        finally
        {
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
        }
    }

    static void Validate(string folder, string tag)
    {
        var version = Path.Combine(folder, "tools", "version.txt");
        if (!File.Exists(Path.Combine(folder, "HookshotBuilder.exe")) || !File.Exists(version) ||
            File.ReadAllText(version).Trim() != tag)
            throw new InvalidDataException("The Builder archive does not contain the requested version.");
    }

    static void WritePointer(string path, string value)
    {
        var partial = path + ".part-" + Guid.NewGuid().ToString("N");
        try
        {
            File.WriteAllText(partial, value + "\n", new System.Text.UTF8Encoding(false));
            File.Move(partial, path, overwrite: true);
        }
        finally { if (File.Exists(partial)) File.Delete(partial); }
    }
}
