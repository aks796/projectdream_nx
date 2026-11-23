// The player's own Project_Dream.exe, loaded as a .NET assembly: its resources (the Android loader, the Vulkan shader
// pack, the game patches bundle, the list of what it downloads) and its conversion code, called by reflection. Nothing
// of Project Dream is copied into this program; without the player's exe it can do nothing.
//
// Copyright 2026 aks796. MIT (see source/LICENSE).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace ProjectDreamBuilder;

internal sealed class ProjectDream
{
	private const BindingFlags Any = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance;

	private readonly Assembly asm;
	private readonly string[] resources;
	private readonly Dictionary<string, Assembly> libraries = new Dictionary<string, Assembly>(StringComparer.OrdinalIgnoreCase);

	internal ProjectDream(string exe)
	{
		byte[] bytes = File.ReadAllBytes(exe);
		if (bytes.Length < 0x200 || bytes[0] != 'M' || bytes[1] != 'Z')
		{
			throw new UserError(exe + " is not Project_Dream.exe (not a Windows program).");
		}
		// Project Dream is IL only, but its PE header says x64, which .NET refuses on an Arm64 computer (Apple Silicon,
		// Arm Linux). The machine field is set to Arm64 in this copy in memory; the IL runs the same either way.
		if (RuntimeInformation.ProcessArchitecture == Architecture.Arm64)
		{
			int pe = BitConverter.ToInt32(bytes, 0x3c);
			if (pe > 0 && pe + 6 < bytes.Length && BitConverter.ToUInt16(bytes, pe + 4) == 0x8664)
			{
				bytes[pe + 4] = 0x64;
				bytes[pe + 5] = 0xAA;
			}
		}
		try
		{
			asm = Assembly.Load(bytes);
		}
		catch (BadImageFormatException)
		{
			throw new UserError(exe + " is not Project_Dream.exe (not a .NET program).");
		}
		resources = asm.GetManifestResourceNames();
		// its own libraries (AssetsTools.NET, Iced) are resources of the exe
		AppDomain.CurrentDomain.AssemblyResolve += (sender, e) =>
		{
			string name = new AssemblyName(e.Name).Name;
			lock (libraries)
			{
				if (libraries.TryGetValue(name, out Assembly loaded))
				{
					return loaded;
				}
				string res = resources.FirstOrDefault(r => r.EndsWith("Libraries." + name + ".dll", StringComparison.Ordinal));
				if (res == null)
				{
					return null;
				}
				return libraries[name] = Assembly.Load(Resource(res));
			}
		};
		if (asm.GetType("ProjectDream.Android.ApkBuilder") == null || FindResource("Shaders.shader-android.bin") == null)
		{
			throw new UserError(exe + " is not a Project Dream with the Android build (no Android builder or shader pack in it).");
		}
		Version = asm.GetName().Version?.ToString() ?? "?";
	}

	internal string Version { get; }

	internal string FindResource(string suffix) => resources.FirstOrDefault(r => r.EndsWith(suffix, StringComparison.Ordinal));

	internal byte[] Resource(string name)
	{
		using Stream s = asm.GetManifestResourceStream(name) ?? throw new UserError("Project_Dream.exe has no " + name);
		using MemoryStream m = new MemoryStream();
		s.CopyTo(m);
		return m.ToArray();
	}

	internal void WriteResource(string suffix, string path)
	{
		string name = FindResource(suffix) ?? throw new UserError("Project_Dream.exe has no " + suffix);
		Directory.CreateDirectory(Path.GetDirectoryName(path));
		File.WriteAllBytes(path, Resource(name));
	}

	internal Type Type(string name) => asm.GetType(name, throwOnError: true);

	internal object Static(string type, string method, params object[] args) => Call(Type(type), null, method, args);

	internal object New(string type, params object[] args)
	{
		try
		{
			return Activator.CreateInstance(Type(type), Any, null, args, null);
		}
		catch (TargetInvocationException e) when (e.InnerException != null)
		{
			throw e.InnerException;
		}
	}

	// an object of the type without running a constructor (for an instance method that uses no instance state)
	internal object Bare(string type) => RuntimeHelpers.GetUninitializedObject(Type(type));

	internal static object Invoke(object obj, string method, params object[] args) => Call(obj.GetType(), obj, method, args);

	internal static object Field(object obj, string name)
	{
		FieldInfo f = obj.GetType().GetField(name, Any) ?? throw new MissingFieldException(obj.GetType().FullName, name);
		return f.GetValue(obj);
	}

	internal static object Property(object obj, string name)
	{
		PropertyInfo p = obj.GetType().GetProperty(name, Any) ?? throw new MissingMemberException(obj.GetType().FullName, name);
		return p.GetValue(obj);
	}

	// a delegate of the given generic delegate type over a static method of Project Dream (e.g. Func<ClassDatabaseFile>)
	internal Delegate StaticDelegate(string type, string method)
	{
		MethodInfo m = Type(type).GetMethod(method, Any, null, System.Type.EmptyTypes, null) ??
		               throw new MissingMethodException(type, method);
		Type func = typeof(Func<>).MakeGenericType(m.ReturnType);
		return Delegate.CreateDelegate(func, m);
	}

	private static object Call(Type t, object obj, string method, object[] args)
	{
		MethodInfo m = t.GetMethods(Any).FirstOrDefault(x => x.Name == method && x.GetParameters().Length == args.Length &&
		                                                     (obj == null) == x.IsStatic) ??
		               throw new MissingMethodException(t.FullName, method);
		try
		{
			return m.Invoke(obj, args);
		}
		catch (TargetInvocationException e) when (e.InnerException != null)
		{
			throw e.InnerException;
		}
	}

	// Project Dream opens files through Windows long paths ("\\?\C:\..."). Elsewhere such a path is relative: a folder
	// link named "\\?\" to "/" in the working folder, which becomes the current directory, makes "\\?\/home/..." the real
	// "/home/...". Nothing to do on Windows.
	internal static void PathsFor(string work)
	{
		if (OperatingSystem.IsWindows())
		{
			return;
		}
		Directory.CreateDirectory(work);
		string link = Path.Combine(work, "\\\\?\\");
		FileSystemInfo info = new DirectoryInfo(link);
		if (info.LinkTarget != "/")
		{
			if (info.Exists || File.Exists(link))
			{
				info.Delete();
			}
			Directory.CreateSymbolicLink(link, "/");
		}
		Environment.CurrentDirectory = work;
	}
}

// a problem with the player's files: said plainly, without a stack trace
internal sealed class UserError : Exception
{
	internal UserError(string message) : base(message) { }
}
