// projectdream_builder: makes sdt.apk, the game file of projectdream_nx (Sonic Dream Team on the Nintendo Switch), from
//   - your own macOS Sonic Dream Team 1.10.1 (the .app, or a .zip of it),
//   - your own Project_Dream.exe (Project Dream by Beatz; its conversion code is what runs),
//   - Unity's Android player, which it downloads from Unity (as Project Dream does).
// It writes SD_CARD/switch/projectdream_nx/sdt.apk (and copies projectdream_nx.nro there when it sits next to the
// builder). Runs on Windows, macOS and Linux; nothing to install.
//
//   projectdream_builder [--mac <game .zip or .app>] [--pd <Project_Dream.exe>] [--out <folder>]
//                        [--unity <AndroidPlayer folder>] [--keep-work]
// Anything not given is asked for (a file can be dragged onto the window).
//
// Copyright 2026 aks796. MIT (see source/LICENSE).
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace ProjectDreamBuilder;

internal static class Program
{
	private const string Port = "projectdream_nx";

	private static int Main(string[] args)
	{
		Console.WriteLine("projectdream_builder " + typeof(Program).Assembly.GetName().Version?.ToString(3) +
		                  ": the game file for " + Port + " (Sonic Dream Team on the Switch)");
		Console.WriteLine();
		bool interactive = args.Length == 0;
		try
		{
			string macPath = Arg(args, "--mac") ?? Ask("Your macOS Sonic Dream Team (the .app, or a .zip of it)");
			string pdPath = Arg(args, "--pd") ?? Ask("Your Project_Dream.exe");
			string outDir = Path.GetFullPath(Arg(args, "--out") ?? Path.Combine(Here(), "SD_CARD"));
			string unityGiven = Arg(args, "--unity");
			// every path made full now: the work folder becomes the current directory later (ProjectDream.PathsFor)
			macPath = Path.GetFullPath(macPath);
			pdPath = Path.GetFullPath(pdPath);
			unityGiven = unityGiven == null ? null : Path.GetFullPath(unityGiven);
			bool keepWork = args.Contains("--keep-work");
			Run(macPath, pdPath, outDir, unityGiven, keepWork);
			if (interactive)
			{
				Pause();
			}
			return 0;
		}
		catch (UserError e)
		{
			Console.WriteLine();
			Console.WriteLine("Stopped: " + e.Message);
		}
		catch (Exception e)
		{
			Console.WriteLine();
			Console.WriteLine("Stopped by an error: " + e);
		}
		if (interactive)
		{
			Pause();
		}
		return 1;
	}

	private static void Run(string macPath, string pdPath, string outDir, string unityGiven, bool keepWork)
	{
		Stopwatch clock = Stopwatch.StartNew();
		// .NET's SHA-256 (Project Dream's shader pack, the download check) uses OpenSSL on Linux; without it the runtime
		// stops in a worker thread. Every desktop Linux (and SteamOS) has it; a bare container may not.
		if (OperatingSystem.IsLinux() && !new[] { "libssl.so.3", "libssl.so.1.1", "libssl.so.10", "libssl.so" }.Any(
			    l => System.Runtime.InteropServices.NativeLibrary.TryLoad(l, out _)))
		{
			throw new UserError("OpenSSL (libssl) is missing; install it (Debian/Ubuntu: sudo apt install libssl3).");
		}
		string port = Path.Combine(outDir, "switch", Port);
		string work = Path.Combine(outDir, "builder_work");
		string apk = Path.Combine(port, "sdt.apk");
		Directory.CreateDirectory(port);
		long free = new DriveInfo(Path.GetPathRoot(outDir)).AvailableFreeSpace;
		if (free < 6L << 30)
		{
			Console.WriteLine("Warning: " + (free >> 20) + " MB free where the output goes; about 6 GB are needed while it works.");
		}

		ProjectDream pd = new ProjectDream(Path.GetFullPath(pdPath));
		Console.WriteLine("Project Dream " + pd.Version);
		using MacGame mac = MacGame.Load(Path.GetFullPath(macPath));
		string version = mac.Version();
		Console.WriteLine("Sonic Dream Team " + (version ?? "(version unknown)") + " for macOS");
		if (version != null && version != "1.10.1")
		{
			throw new UserError("This is version " + version + "; Project Dream and the port are made for 1.10.1.");
		}
		ProjectDream.PathsFor(work);
		Steps steps = new Steps(pd, mac, work);

		Console.WriteLine("[1/6] Unity's Android player");
		string unity = UnityPlayer.Get(pd, Path.Combine(work, "unity"), unityGiven);
		Console.WriteLine("[2/6] Vulkan shaders");
		steps.Shaders();
		Console.WriteLine("[3/6] Addressables catalog");
		Console.WriteLine("  " + steps.Catalog() + " bundles' checksums updated");
		Console.WriteLine("[4/6] globalgamemanagers");
		steps.GameManagers();
		Console.WriteLine("[5/6] GameAssembly and libunity");
		string ga = steps.GameAssembly();
		string libunity = steps.LibUnity(unity);
		Console.WriteLine("[6/6] sdt.apk");
		long size = steps.Apk(unity, libunity, ga, apk);

		string nro = Path.Combine(Here(), Port + ".nro");
		if (File.Exists(nro))
		{
			File.Copy(nro, Path.Combine(port, Port + ".nro"), true);
		}
		Environment.CurrentDirectory = outDir;
		if (!keepWork)
		{
			foreach (string dir in Directory.GetDirectories(work).Where(d => Path.GetFileName(d) != "unity"))
			{
				DeleteTree(dir);
			}
			foreach (string file in Directory.GetFiles(work))
			{
				File.Delete(file);
			}
		}
		Console.WriteLine();
		Console.WriteLine("Done in " + (int)clock.Elapsed.TotalMinutes + " min " + clock.Elapsed.Seconds + " s: " + apk + " (" + (size >> 20) + " MB)");
		Console.WriteLine("Copy the switch folder in " + outDir + " to the root of your SD card" +
		                  (File.Exists(Path.Combine(port, Port + ".nro")) ? "." : ", with " + Port + ".nro from the release in switch/" + Port + "/."));
		Console.WriteLine("(" + Path.Combine(work, "unity") + " keeps Unity's files for another run; delete builder_work when you are done.)");
	}

	// a folder tree without following links (the work folder's "\\?\" link points at "/")
	private static void DeleteTree(string dir)
	{
		DirectoryInfo d = new DirectoryInfo(dir);
		if (d.LinkTarget != null)
		{
			d.Delete();
			return;
		}
		foreach (DirectoryInfo sub in d.GetDirectories())
		{
			DeleteTree(sub.FullName);
		}
		foreach (FileInfo f in d.GetFiles())
		{
			f.Delete();
		}
		d.Delete();
	}

	private static string Here() => Path.GetDirectoryName(Environment.ProcessPath ?? AppContext.BaseDirectory) ?? ".";

	private static string Arg(string[] args, string name)
	{
		int i = Array.IndexOf(args, name);
		return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
	}

	// a path typed or dropped onto the window: quotes and the shell's backslash escapes taken off
	private static string Ask(string what)
	{
		while (true)
		{
			Console.Write(what + ": ");
			string line = Console.ReadLine();
			if (line == null)
			{
				throw new UserError("No input.");
			}
			line = line.Trim();
			if (line.Length >= 2 && (line[0] == '"' || line[0] == '\'') && line[line.Length - 1] == line[0])
			{
				line = line.Substring(1, line.Length - 2);
			}
			else if (!OperatingSystem.IsWindows())
			{
				line = System.Text.RegularExpressions.Regex.Replace(line, "\\\\(.)", "$1");
			}
			if (File.Exists(line) || Directory.Exists(line))
			{
				return line;
			}
			Console.WriteLine("  nothing at " + line);
		}
	}

	private static void Pause()
	{
		Console.WriteLine();
		Console.Write("Press Enter to close.");
		Console.ReadLine();
	}
}
