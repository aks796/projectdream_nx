// The player's macOS Sonic Dream Team: the .app folder or a .zip of it. Paths are relative to the app's Contents/,
// '/' separated.
//
// Copyright 2026 aks796. MIT (see source/LICENSE).
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.RegularExpressions;

namespace ProjectDreamBuilder;

internal abstract class MacGame : IDisposable
{
	internal const string Data = "Resources/Data/";

	internal abstract IEnumerable<string> Files { get; }

	internal abstract Stream Open(string rel);

	public abstract void Dispose();

	internal static MacGame Load(string path)
	{
		if (File.Exists(path))
		{
			return new Zip(path);
		}
		if (Directory.Exists(path))
		{
			return new Folder(path);
		}
		throw new UserError("Nothing at " + path);
	}

	internal void Copy(string rel, string dest)
	{
		Directory.CreateDirectory(Path.GetDirectoryName(dest));
		using Stream i = Open(rel);
		using FileStream o = File.Create(dest);
		i.CopyTo(o, 1 << 20);
	}

	// CFBundleShortVersionString of Contents/Info.plist (XML plists only), or null
	internal string Version()
	{
		if (!Files.Contains("Info.plist"))
		{
			return null;
		}
		using StreamReader r = new StreamReader(Open("Info.plist"));
		string text = r.ReadToEnd();
		Match m = Regex.Match(text, "<key>CFBundleShortVersionString</key>\\s*<string>([^<]*)</string>");
		return m.Success ? m.Groups[1].Value : null;
	}

	private sealed class Zip : MacGame
	{
		private readonly ZipArchive zip;
		private readonly Dictionary<string, ZipArchiveEntry> entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.Ordinal);

		internal Zip(string path)
		{
			try
			{
				zip = ZipFile.OpenRead(path);
			}
			catch (InvalidDataException)
			{
				throw new UserError(path + " is not a .zip file.");
			}
			// entry names with '/' (some Windows zip tools write backslashes); the app's Contents/ may be at the top
			static string Name(ZipArchiveEntry e) => e.FullName.Replace('\\', '/');
			string prefix = null;
			foreach (ZipArchiveEntry e in zip.Entries)
			{
				string n = "/" + Name(e);
				int i = n.IndexOf("/Contents/" + Data + "globalgamemanagers", StringComparison.Ordinal);
				if (i >= 0 && !n.StartsWith("/__MACOSX/", StringComparison.Ordinal) && n.EndsWith("/globalgamemanagers", StringComparison.Ordinal))
				{
					prefix = (n.Substring(0, i) + "/Contents/").Substring(1);
					break;
				}
			}
			if (prefix == null)
			{
				throw new UserError("No macOS Sonic Dream Team app in " + path + ".");
			}
			foreach (ZipArchiveEntry e in zip.Entries)
			{
				string n = Name(e);
				if (n.StartsWith(prefix, StringComparison.Ordinal) && !n.EndsWith("/", StringComparison.Ordinal))
				{
					entries[n.Substring(prefix.Length)] = e;
				}
			}
		}

		internal override IEnumerable<string> Files => entries.Keys;

		internal override Stream Open(string rel) => entries[rel].Open();

		public override void Dispose() => zip.Dispose();
	}

	private sealed class Folder : MacGame
	{
		private readonly string root;

		internal Folder(string app)
		{
			root = Directory.Exists(Path.Combine(app, "Contents")) ? Path.Combine(app, "Contents") : app;
			if (!File.Exists(Path.Combine(root, "Resources", "Data", "globalgamemanagers")))
			{
				throw new UserError("No macOS Sonic Dream Team app at " + app + ".");
			}
		}

		internal override IEnumerable<string> Files => Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
			.Select(f => Path.GetRelativePath(root, f).Replace('\\', '/'));

		internal override Stream Open(string rel) => File.OpenRead(Path.Combine(root, rel));

		public override void Dispose() { }
	}
}
