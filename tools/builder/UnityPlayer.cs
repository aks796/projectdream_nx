// Unity's Android player 2022.3.54f1, the three files of it the APK needs, from Unity's own download: the same
// package Project Dream downloads (its address and SHA-256 are read from the player's Project_Dream.exe). The package
// (a xar archive whose Payload is a gzip'd cpio archive, about 675 MB) is read as it downloads, nothing of it saved but
// those files; the whole download is checked against the SHA-256 at the end.
//
// Copyright 2026 aks796. MIT (see source/LICENSE).
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Xml.Linq;

namespace ProjectDreamBuilder;

internal static class UnityPlayer
{
	internal const string UnityDefaultResources = "Data/unity default resources";
	internal const string LibUnity = "Variations/il2cpp/Release/Libs/arm64-v8a/libunity.so";
	internal const string LibMain = "Variations/il2cpp/Release/Libs/arm64-v8a/libmain.so";
	private static readonly string[] Wanted = { UnityDefaultResources, LibUnity, LibMain };

	private static bool Complete(string folder) => Wanted.All(w => File.Exists(Path.Combine(folder, w)));

	// the folder that holds the files: the one given, the cache, or the cache filled from Unity's download
	internal static string Get(ProjectDream pd, string cache, string given)
	{
		if (given != null)
		{
			if (!Complete(given))
			{
				throw new UserError(given + " is not Unity's AndroidPlayer folder (it needs " + string.Join(", ", Wanted) + ").");
			}
			return given;
		}
		(string url, string sha256) = Source(pd);
		string stamp = Path.Combine(cache, "source.txt");
		if (Complete(cache) && File.Exists(stamp) && File.ReadAllText(stamp).Trim() == sha256)
		{
			Console.WriteLine("  already downloaded");
			return cache;
		}
		if (Directory.Exists(cache))
		{
			Directory.Delete(cache, true);
		}
		Directory.CreateDirectory(cache);
		Download(url, sha256, cache);
		File.WriteAllText(stamp, sha256 + "\n");
		return cache;
	}

	private static (string url, string sha256) Source(ProjectDream pd)
	{
		string name = pd.FindResource("tool-downloads.json") ?? throw new UserError("Project_Dream.exe has no list of downloads.");
		using JsonDocument doc = JsonDocument.Parse(pd.Resource(name));
		foreach (JsonElement e in doc.RootElement.EnumerateArray())
		{
			string url = e.GetProperty("url").GetString();
			if (url.Contains("Android-Support-for-Editor", StringComparison.Ordinal) && url.EndsWith(".pkg", StringComparison.Ordinal))
			{
				return (url, e.GetProperty("sha256").GetString().ToLowerInvariant());
			}
		}
		throw new UserError("Project_Dream.exe does not name Unity's Android player download.");
	}

	private static void Download(string url, string sha256, string cache)
	{
		Console.WriteLine("  " + url);
		using HttpClient http = new HttpClient { Timeout = TimeSpan.FromHours(2) };
		http.DefaultRequestHeaders.UserAgent.ParseAdd("projectdream_builder/1.0");
		using HttpResponseMessage resp = http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead).GetAwaiter().GetResult();
		if (!resp.IsSuccessStatusCode)
		{
			throw new UserError("Unity's download answered " + (int)resp.StatusCode + " " + resp.ReasonPhrase + ".");
		}
		long total = resp.Content.Headers.ContentLength ?? 0;
		using Stream net = resp.Content.ReadAsStream();
		using Hashing all = new Hashing(net, total);
		HashSet<string> found = ReadPkg(all, cache);
		all.Drain();
		string got = all.Sha256();
		if (got != sha256)
		{
			Directory.Delete(cache, true);
			throw new UserError("Unity's download did not match its checksum (" + got + "); try again.");
		}
		if (found.Count != Wanted.Length)
		{
			throw new UserError("Unity's package did not have " + string.Join(", ", Wanted.Except(found)) + ".");
		}
	}

	private static HashSet<string> ReadPkg(Hashing s, string cache)
	{
		byte[] head = new byte[28];
		s.ReadExactly(head);
		if (Encoding.ASCII.GetString(head, 0, 4) != "xar!")
		{
			throw new UserError("Unity's download is not a macOS package.");
		}
		int headerSize = BigEndian16(head, 4);
		long tocCompressed = (long)BigEndian64(head, 8);
		Skip(s, headerSize - 28);
		byte[] tocZ = new byte[tocCompressed];
		s.ReadExactly(tocZ);
		XDocument toc;
		using (ZLibStream z = new ZLibStream(new MemoryStream(tocZ), CompressionMode.Decompress))
		{
			toc = XDocument.Load(z);
		}
		XElement payload = toc.Descendants("file")
			.Where(f => (string)f.Element("name") == "Payload" && f.Element("data") != null)
			.OrderByDescending(f => (long)f.Element("data").Element("length"))
			.FirstOrDefault() ?? throw new UserError("Unity's package has no Payload.");
		XElement data = payload.Element("data");
		long offset = (long)data.Element("offset"), length = (long)data.Element("length");
		string encoding = (string)data.Element("encoding")?.Attribute("style") ?? "application/octet-stream";
		if (encoding != "application/octet-stream")
		{
			throw new UserError("Unity's package stores its Payload as " + encoding + ", which this builder does not read.");
		}
		long heap = headerSize + tocCompressed;
		Skip(s, heap + offset - s.Done);
		using Limited body = new Limited(s, length);
		using GZipStream cpio = new GZipStream(body, CompressionMode.Decompress);
		HashSet<string> found = ReadCpio(cpio, cache);
		body.Drain();
		return found;
	}

	// odc ("070707") or newc ("070701"/"070702") entries until TRAILER!!!; the wanted files written to the cache
	private static HashSet<string> ReadCpio(Stream s, string cache)
	{
		HashSet<string> found = new HashSet<string>(StringComparer.Ordinal);
		byte[] magic = new byte[6];
		while (true)
		{
			s.ReadExactly(magic);
			string m = Encoding.ASCII.GetString(magic);
			long nameSize, fileSize;
			int mode;
			bool newc;
			if (m == "070707")
			{
				byte[] h = new byte[70];
				s.ReadExactly(h);
				string t = Encoding.ASCII.GetString(h);
				mode = Convert.ToInt32(t.Substring(12, 6), 8);
				nameSize = Convert.ToInt64(t.Substring(53, 6), 8);
				fileSize = Convert.ToInt64(t.Substring(59, 11), 8);
				newc = false;
			}
			else if (m == "070701" || m == "070702")
			{
				byte[] h = new byte[104];
				s.ReadExactly(h);
				string t = Encoding.ASCII.GetString(h);
				mode = Convert.ToInt32(t.Substring(8, 8), 16);
				fileSize = Convert.ToInt64(t.Substring(48, 8), 16);
				nameSize = Convert.ToInt64(t.Substring(88, 8), 16);
				newc = true;
			}
			else
			{
				throw new UserError("Unity's package has an unexpected archive inside (" + m + ").");
			}
			byte[] nameBytes = new byte[nameSize];
			s.ReadExactly(nameBytes);
			if (newc)
			{
				Skip(s, Pad4(110 + nameSize));
			}
			string name = Encoding.UTF8.GetString(nameBytes).TrimEnd('\0');
			if (name == "TRAILER!!!")
			{
				return found;
			}
			if (name.StartsWith("./", StringComparison.Ordinal))
			{
				name = name.Substring(2);
			}
			if ((mode & 0xF000) == 0x8000 && Wanted.Contains(name))
			{
				string dest = Path.Combine(cache, name);
				Directory.CreateDirectory(Path.GetDirectoryName(dest));
				using (FileStream f = File.Create(dest))
				{
					Copy(s, f, fileSize);
				}
				found.Add(name);
			}
			else
			{
				Skip(s, fileSize);
			}
			if (newc)
			{
				Skip(s, Pad4(fileSize));
			}
		}
	}

	private static long Pad4(long n) => (4 - n % 4) % 4;

	private static int BigEndian16(byte[] b, int o) => b[o] << 8 | b[o + 1];

	private static ulong BigEndian64(byte[] b, int o)
	{
		ulong v = 0;
		for (int i = 0; i < 8; i++)
		{
			v = v << 8 | b[o + i];
		}
		return v;
	}

	private static readonly byte[] Scratch = new byte[1 << 20];

	private static void Skip(Stream s, long n)
	{
		if (n < 0)
		{
			throw new UserError("Unity's package is laid out unexpectedly.");
		}
		while (n > 0)
		{
			int got = s.Read(Scratch, 0, (int)Math.Min(n, Scratch.Length));
			if (got <= 0)
			{
				throw new EndOfStreamException();
			}
			n -= got;
		}
	}

	private static void Copy(Stream s, Stream o, long n)
	{
		while (n > 0)
		{
			int got = s.Read(Scratch, 0, (int)Math.Min(n, Scratch.Length));
			if (got <= 0)
			{
				throw new EndOfStreamException();
			}
			o.Write(Scratch, 0, got);
			n -= got;
		}
	}

	// the download, hashed as it is read, with a progress line every 25 MB
	private sealed class Hashing : Stream
	{
		private readonly Stream inner;
		private readonly long total;
		private readonly IncrementalHash sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
		private long nextReport;

		internal Hashing(Stream inner, long total)
		{
			this.inner = inner;
			this.total = total;
		}

		internal long Done { get; private set; }

		public override int Read(byte[] buffer, int offset, int count)
		{
			int n = inner.Read(buffer, offset, count);
			if (n > 0)
			{
				sha.AppendData(buffer, offset, n);
				Done += n;
				if (Done >= nextReport)
				{
					nextReport += 25 << 20;
					Console.Write("\r  " + (Done >> 20) + (total > 0 ? " / " + (total >> 20) : "") + " MB   ");
				}
			}
			return n;
		}

		internal void Drain()
		{
			byte[] b = new byte[1 << 20];
			while (Read(b, 0, b.Length) > 0) { }
			Console.WriteLine();
		}

		internal string Sha256() => Convert.ToHexString(sha.GetHashAndReset()).ToLowerInvariant();

		public override bool CanRead => true;
		public override bool CanSeek => false;
		public override bool CanWrite => false;
		public override long Length => throw new NotSupportedException();
		public override long Position { get => Done; set => throw new NotSupportedException(); }
		public override void Flush() { }
		public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
		public override void SetLength(long value) => throw new NotSupportedException();
		public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

		protected override void Dispose(bool disposing)
		{
			if (disposing)
			{
				sha.Dispose();
			}
			base.Dispose(disposing);
		}
	}

	// at most n bytes of the stream under it (left open)
	private sealed class Limited : Stream
	{
		private readonly Stream inner;
		private long left;

		internal Limited(Stream inner, long n)
		{
			this.inner = inner;
			left = n;
		}

		public override int Read(byte[] buffer, int offset, int count)
		{
			if (left <= 0)
			{
				return 0;
			}
			int n = inner.Read(buffer, offset, (int)Math.Min(count, left));
			left -= Math.Max(n, 0);
			return n;
		}

		internal void Drain()
		{
			byte[] b = new byte[1 << 20];
			while (Read(b, 0, b.Length) > 0) { }
		}

		public override bool CanRead => true;
		public override bool CanSeek => false;
		public override bool CanWrite => false;
		public override long Length => throw new NotSupportedException();
		public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
		public override void Flush() { }
		public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
		public override void SetLength(long value) => throw new NotSupportedException();
		public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
	}
}
