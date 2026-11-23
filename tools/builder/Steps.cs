// The conversion, step by step: what Project Dream's Android build does to the macOS game, run with Project Dream's
// own code (from the player's exe) where it can be, minus what the Switch does not need (texture transcoding: the
// Tegra X1 samples the Mac build's BC textures as they are; the video decode shader: the port decodes video itself),
// and with the APK laid out the way the Switch port reads it.
//
// Copyright 2026 aks796. MIT (see source/LICENSE).
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;

namespace ProjectDreamBuilder;

internal sealed class Steps
{
	private const string D = MacGame.Data;
	private static readonly string[] Skipped = { ".txt", ".log", ".md", ".pdb" };

	private readonly ProjectDream pd;
	private readonly MacGame mac;
	private readonly HashSet<string> files;
	private readonly string work, data, converted;
	private readonly Delegate classDatabase; // Project Dream's Func<ClassDatabaseFile> for Unity 2022.3

	internal Steps(ProjectDream pd, MacGame mac, string work)
	{
		this.pd = pd;
		this.mac = mac;
		this.work = work;
		files = new HashSet<string>(mac.Files, StringComparer.Ordinal);
		data = Path.Combine(work, "data");
		converted = Path.Combine(work, "converted");
		Directory.CreateDirectory(data);
		Directory.CreateDirectory(converted);
		classDatabase = pd.StaticDelegate("ProjectDream.Assets.ClassDatabases", "Unity");
	}

	// 1. the 276 Shader objects in 81 files get Project Dream's Vulkan (SPIR-V) programs
	internal void Shaders()
	{
		object shaders = pd.New("ProjectDream.Shaders.ShaderRead", "shader-android");
		List<string> targets = ((IEnumerable<string>)ProjectDream.Property(shaders, "Files")).OrderBy(x => x, StringComparer.Ordinal).ToList();
		foreach (string rel in targets.Concat(new[] { "globalgamemanagers", "StreamingAssets/aa/catalog.json", "StreamingAssets/aa/settings.json" }))
		{
			if (!files.Contains(D + rel))
			{
				throw new UserError("The macOS game has no " + rel + " (is it version 1.10.1?).");
			}
			if (!File.Exists(Path.Combine(data, rel)))
			{
				mac.Copy(D + rel, Path.Combine(data, rel));
			}
		}
		int errors = 0, done = 0;
		Parallel.ForEach(targets, new ParallelOptions { MaxDegreeOfParallelism = Math.Max(1, Environment.ProcessorCount - 1) }, rel =>
		{
			try
			{
				object doc = pd.New("ProjectDream.Assets.AssetDocument", Path.Combine(data, rel), classDatabase);
				try
				{
					if ((bool)ProjectDream.Invoke(shaders, "Apply", rel, doc))
					{
						string dest = Path.Combine(converted, rel);
						Directory.CreateDirectory(Path.GetDirectoryName(dest));
						ProjectDream.Invoke(doc, "Save", dest);
					}
				}
				finally
				{
					((IDisposable)doc).Dispose();
				}
			}
			catch (Exception e)
			{
				Interlocked.Increment(ref errors);
				Console.Error.WriteLine("  " + rel + ": " + e.Message);
			}
			int k = Interlocked.Increment(ref done);
			if (k % 10 == 0 || k == targets.Count)
			{
				Console.WriteLine("  " + k + " / " + targets.Count);
			}
		});
		if (errors > 0)
		{
			throw new UserError(errors + " of the game's files could not be converted (see above).");
		}
	}

	// 2. the Addressables catalog: the new checksums and sizes of the bundles step 1 changed (Project Dream's), and the
	// settings: the one local catalog, never updated from the network
	internal int Catalog()
	{
		string aa = Path.Combine(data, "StreamingAssets", "aa"), outAa = Path.Combine(converted, "StreamingAssets", "aa");
		Directory.CreateDirectory(outAa);
		// Project Dream looks for the changed bundles under Windows paths ("StandaloneOSX\\x.bundle"); elsewhere a
		// backslash is part of a file name, so each converted bundle gets a hard link by that name for this step
		List<string> links = new List<string>();
		if (!OperatingSystem.IsWindows())
		{
			foreach (string f in Directory.EnumerateFiles(outAa, "*.bundle", SearchOption.AllDirectories))
			{
				string rel = Path.GetRelativePath(outAa, f);
				if (rel.Contains('/'))
				{
					string link = Path.Combine(outAa, rel.Replace('/', '\\'));
					if (File.Exists(link))
					{
						File.Delete(link);
					}
					if (Unix.Link(f, link) != 0)
					{
						throw new IOException("Could not link " + f + " (errno " + System.Runtime.InteropServices.Marshal.GetLastPInvokeError() + ")");
					}
					links.Add(link);
				}
			}
		}
		int bundles;
		try
		{
			bundles = (int)pd.Static("ProjectDream.Android.Addressables", "UpdateCatalog",
				Path.Combine(aa, "catalog.json"), converted, Path.Combine(outAa, "catalog.json"));
		}
		finally
		{
			foreach (string link in links)
			{
				File.Delete(link);
			}
		}
		JsonObject settings = JsonNode.Parse(File.ReadAllText(Path.Combine(aa, "settings.json"), Encoding.UTF8)).AsObject();
		List<JsonNode> main = settings["m_CatalogLocations"].AsArray()
			.Where(l => l["m_Keys"].AsArray().Any(k => (string)k == "AddressablesMainContentCatalog"))
			.ToList();
		if (main.Count != 1)
		{
			throw new UserError("The game's Addressables settings do not have one local catalog.");
		}
		JsonObject local = main[0].DeepClone().AsObject();
		local["m_InternalId"] = "{UnityEngine.AddressableAssets.Addressables.RuntimePath}/catalog.json";
		local["m_Dependencies"] = new JsonArray();
		settings["m_CatalogLocations"] = new JsonArray(local);
		settings["m_DisableCatalogUpdateOnStart"] = true;
		File.WriteAllText(Path.Combine(outAa, "settings.json"), settings.ToJsonString(), new UTF8Encoding(false));
		return bundles;
	}

	// 3. globalgamemanagers: Vulkan only (the one API Project Dream's shaders have), GPU skinning off, the blit type
	// Android needs, no Swappy (Android's frame pacing library; the port paces the frames), and the shader defines of
	// Unity's Android compilers
	internal void GameManagers()
	{
		string source = Path.Combine(data, "globalgamemanagers"), dest = Path.Combine(converted, "globalgamemanagers");
		object doc = pd.New("ProjectDream.Assets.AssetDocument", source, classDatabase);
		try
		{
			foreach (object item in (IEnumerable)ProjectDream.Invoke(doc, "Objects", new[] { 141, 129, 30 }))
			{
				dynamic info = ProjectDream.Field(item, "Info");
				dynamic tree = ProjectDream.Field(item, "Tree");
				switch ((int)info.TypeId)
				{
				case 141: // BuildSettings
					pd.Static("ProjectDream.Assets.Fields", "SetInts", (object)tree["m_GraphicsAPIs"], new[] { 21 });
					break;
				case 129: // PlayerSettings
					tree["androidBlitType"].AsInt = 1;
					tree["gpuSkinning"].AsBool = false;
					tree["androidUseSwappy"].AsBool = false;
					break;
				case 30: // GraphicsSettings
					IList defines = (IList)pd.Static("ProjectDream.Assets.Fields", "Array", (object)tree["m_ShaderDefinesPerShaderCompiler"]);
					if (defines.Count == 0)
					{
						break;
					}
					foreach (int platform in new[] { 9, 18 })
					{
						bool has = false;
						foreach (dynamic d in defines)
						{
							has |= (int)d["shaderPlatform"].AsInt == platform;
						}
						if (!has)
						{
							dynamic copy = ((dynamic)defines[0]).Clone();
							copy["shaderPlatform"].AsInt = platform;
							defines.Add(copy);
						}
					}
					break;
				}
				info.SetNewData(tree);
			}
			ProjectDream.Invoke(doc, "Save", dest);
		}
		finally
		{
			((IDisposable)doc).Dispose();
		}
	}

	// 4. GameAssembly.dylib's arm64 slice with Project Dream's code patches
	internal string GameAssembly()
	{
		string dylib = Path.Combine(work, "GameAssembly.dylib");
		if (!files.Contains("Frameworks/GameAssembly.dylib"))
		{
			throw new UserError("The macOS game has no Frameworks/GameAssembly.dylib.");
		}
		mac.Copy("Frameworks/GameAssembly.dylib", dylib);
		string thin = (string)pd.Static("ProjectDream.Android.ApkBuilder", "ThinArm64", dylib, Path.Combine(work, "GameAssembly.arm64"));
		string patched = (string)pd.Static("ProjectDream.Android.ApkBuilder", "PatchGameAssembly", thin);
		File.Delete(dylib);
		return patched;
	}

	// 5. Unity's libunity.so with Project Dream's two patches that let it read the Mac bundles
	internal string LibUnity(string unity)
	{
		string dest = Path.Combine(work, "libunity.so");
		object builder = pd.Bare("ProjectDream.Android.ApkBuilder");
		return (string)ProjectDream.Invoke(builder, "PatchLibUnity", Path.Combine(unity, UnityPlayer.LibUnity), dest);
	}

	// 6. the APK: stored entries, 4 KB aligned, laid out as Unity's Android player reads one
	internal long Apk(string unity, string libunity, string gameAssembly, string apk)
	{
		string loader = Path.Combine(work, "pd", "libil2cpp.so"), patches = Path.Combine(work, "pd", "gamepatches.bundle");
		pd.WriteResource("Android/prebuilt/libil2cpp.so", loader);
		pd.WriteResource("Android/resources/gamepatches.bundle", patches);
		string guid = "00000000000000000000000000000000";
		using (StreamReader r = new StreamReader(mac.Open(D + "boot.config")))
		{
			string line;
			while ((line = r.ReadLine()) != null)
			{
				if (line.StartsWith("build-guid=", StringComparison.Ordinal))
				{
					guid = line.Substring(line.IndexOf('=') + 1).Trim();
				}
			}
		}
		string tmp = apk + ".tmp";
		Directory.CreateDirectory(Path.GetDirectoryName(apk));
		using (ApkZip zip = new ApkZip(tmp))
		{
			void Add(string rel, string name)
			{
				string conv = Path.Combine(converted, rel.Substring(D.Length));
				if (rel.StartsWith(D, StringComparison.Ordinal) && File.Exists(conv))
				{
					zip.AddFile(conv, name);
					return;
				}
				using Stream s = mac.Open(rel);
				zip.Add(s, name);
			}
			bool Kept(string rel) => !Skipped.Contains(Path.GetExtension(rel).ToLowerInvariant());
			zip.AddFile(libunity, "lib/arm64-v8a/libunity.so");
			zip.AddFile(Path.Combine(unity, UnityPlayer.LibMain), "lib/arm64-v8a/libmain.so");
			zip.AddFile(loader, "lib/arm64-v8a/libil2cpp.so");
			foreach (string rel in files.Where(f => f.StartsWith(D, StringComparison.Ordinal) && f.IndexOf('/', D.Length) < 0)
				         .OrderBy(x => x, StringComparer.Ordinal).Where(Kept))
			{
				Add(rel, "assets/bin/Data/" + rel.Substring(D.Length));
			}
			Add(D + "Resources/unity_builtin_extra", "assets/bin/Data/Resources/unity_builtin_extra");
			foreach (string rel in files.Where(f => f.StartsWith(D + "il2cpp_data/", StringComparison.Ordinal))
				         .OrderBy(x => x, StringComparer.Ordinal).Where(Kept))
			{
				Add(rel, "assets/bin/Data/Managed/" + rel.Substring((D + "il2cpp_data/").Length));
			}
			zip.AddFile(Path.Combine(unity, UnityPlayer.UnityDefaultResources), "assets/bin/Data/unity default resources");
			zip.AddFile(gameAssembly, "assets/bin/Data/GameAssembly.arm64");
			zip.AddFile(patches, "assets/gamepatches.bundle");
			zip.AddBytes(Encoding.ASCII.GetBytes(guid), "assets/bin/Data/unity_app_guid", align: 4); // (4: as the tested APKs have it)
			List<string> streaming = files.Where(f => f.StartsWith(D + "StreamingAssets/", StringComparison.Ordinal))
				.OrderBy(x => x, StringComparer.Ordinal).ToList();
			int k = 0;
			foreach (string rel in streaming)
			{
				if (Kept(rel))
				{
					Add(rel, "assets/" + rel.Substring((D + "StreamingAssets/").Length));
				}
				if (++k % 500 == 0 || k == streaming.Count)
				{
					Console.WriteLine("  " + k + " / " + streaming.Count + " files, " + (zip.TotalBytes >> 20) + " MB");
				}
			}
		}
		if (File.Exists(apk))
		{
			File.Delete(apk);
		}
		File.Move(tmp, apk);
		return new FileInfo(apk).Length;
	}
}

internal static class Unix
{
	// "libc" is not a name every Linux resolves (libc.so can be a linker script); glibc's is libc.so.6
	static Unix()
	{
		System.Runtime.InteropServices.NativeLibrary.SetDllImportResolver(typeof(Unix).Assembly, (name, asm, path) =>
			name == "libc" && OperatingSystem.IsLinux() &&
			System.Runtime.InteropServices.NativeLibrary.TryLoad("libc.so.6", out IntPtr h) ? h : IntPtr.Zero);
	}

	internal static int Link(string existing, string newName) => link(existing, newName);

	[System.Runtime.InteropServices.DllImport("libc", SetLastError = true)]
	private static extern int link(string existing, string newName);
}
