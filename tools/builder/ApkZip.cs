// The APK: a zip of stored entries, each one's data aligned (4 KB here) by padding in its local header's extra field,
// as Android's zipalign does. Sizes stay under 4 GB (no zip64; the game is about 2.4 GB).
//
// Copyright 2026 aks796. MIT (see source/LICENSE).
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Hashing;
using System.Text;

namespace ProjectDreamBuilder;

internal sealed class ApkZip : IDisposable
{
	private const ushort DosDate = (2024 - 1980) << 9 | 1 << 5 | 1; // 2024-01-01, a fixed date: the same input, the same bytes
	private const uint UnixFileRw = 0x81A40000; // regular file, rw-r--r--, as external attributes

	private sealed class Entry
	{
		internal byte[] Name;
		internal ushort Flags;
		internal uint Crc, Size, Offset;
	}

	private readonly FileStream stream;
	private readonly BinaryWriter w;
	private readonly List<Entry> entries = new List<Entry>();
	private readonly byte[] buffer = new byte[1 << 20];

	internal ApkZip(string path)
	{
		stream = File.Create(path);
		w = new BinaryWriter(stream);
	}

	internal long TotalBytes { get; private set; }

	internal void AddFile(string path, string name, int align = 4096)
	{
		using FileStream f = File.OpenRead(path);
		Add(f, name, align);
	}

	internal void AddBytes(byte[] data, string name, int align = 4096) => Add(new MemoryStream(data, false), name, align);

	internal long Add(Stream input, string name, int align = 4096)
	{
		if (stream.Position > uint.MaxValue)
		{
			throw new IOException("The APK passed 4 GB");
		}
		Entry e = new Entry { Name = Encoding.UTF8.GetBytes(name), Offset = (uint)stream.Position };
		foreach (char c in name)
		{
			if (c >= 0x80)
			{
				e.Flags = 0x800; // UTF-8 name
			}
		}
		// padding: an extra field (id 0xFFFF) long enough to put the data on the boundary
		int pad = (int)((align - (stream.Position + 30 + e.Name.Length) % align) % align);
		if (pad > 0 && pad < 4)
		{
			pad += align;
		}
		byte[] extra = new byte[pad];
		if (pad > 0)
		{
			extra[0] = 0xFF;
			extra[1] = 0xFF;
			BitConverter.GetBytes((ushort)(pad - 4)).CopyTo(extra, 2);
		}
		long header = stream.Position;
		w.Write(0x04034b50u);
		w.Write((ushort)20);
		w.Write(e.Flags);
		w.Write((ushort)0); // stored
		w.Write((ushort)0); // time
		w.Write(DosDate);
		w.Write(0u); // crc, sizes: filled in below
		w.Write(0u);
		w.Write(0u);
		w.Write((ushort)e.Name.Length);
		w.Write((ushort)extra.Length);
		w.Write(e.Name);
		w.Write(extra);
		w.Flush();
		Crc32 crc = new Crc32();
		long size = 0;
		int n;
		while ((n = input.Read(buffer, 0, buffer.Length)) > 0)
		{
			crc.Append(new ReadOnlySpan<byte>(buffer, 0, n));
			stream.Write(buffer, 0, n);
			size += n;
		}
		if (size > uint.MaxValue || stream.Position > uint.MaxValue)
		{
			throw new IOException("A file passed 4 GB: " + name);
		}
		e.Crc = crc.GetCurrentHashAsUInt32();
		e.Size = (uint)size;
		long end = stream.Position;
		stream.Position = header + 14;
		w.Write(e.Crc);
		w.Write(e.Size);
		w.Write(e.Size);
		w.Flush();
		stream.Position = end;
		entries.Add(e);
		TotalBytes += size;
		return size;
	}

	public void Dispose()
	{
		long cd = stream.Position;
		foreach (Entry e in entries)
		{
			w.Write(0x02014b50u);
			w.Write((ushort)20); // made by
			w.Write((ushort)20); // needed
			w.Write(e.Flags);
			w.Write((ushort)0);
			w.Write((ushort)0);
			w.Write(DosDate);
			w.Write(e.Crc);
			w.Write(e.Size);
			w.Write(e.Size);
			w.Write((ushort)e.Name.Length);
			w.Write((ushort)0); // extra
			w.Write((ushort)0); // comment
			w.Write((ushort)0); // disk
			w.Write((ushort)0); // internal attributes
			w.Write(UnixFileRw);
			w.Write(e.Offset);
			w.Write(e.Name);
		}
		long cdSize = stream.Position - cd;
		if (entries.Count > 0xFFFF || cd > uint.MaxValue)
		{
			throw new IOException("The APK is too large for a zip without zip64");
		}
		w.Write(0x06054b50u);
		w.Write((ushort)0);
		w.Write((ushort)0);
		w.Write((ushort)entries.Count);
		w.Write((ushort)entries.Count);
		w.Write((uint)cdSize);
		w.Write((uint)cd);
		w.Write((ushort)0);
		w.Flush();
		stream.Dispose();
	}
}
