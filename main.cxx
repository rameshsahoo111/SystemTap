// systemtap translator/driver
// Copyright (C) 2005-2007 Red Hat Inc.
// Copyright (C) 2005 IBM Corp.
// Copyright (C) 2006 Intel Corporation.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"
#include "staptree.h"
#include "parse.h"
#include "elaborate.h"
#include "translate.h"
#include "buildrun.h"
#include "session.h"
#include "hash.h"
#include "cache.h"
#include "util.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cerrno>
#include <cstdlib>

extern "C" {
#include <glob.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/times.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <elfutils/libdwfl.h>
}

using namespace std;


void
version ()
{
  clog
    << "SystemTap translator/driver "
    << "(version " << VERSION << " built " << DATE << ")" << endl
    << "(Using " << dwfl_version (NULL) << " libraries.)" << endl
    << "Copyright (C) 2005-2007 Red Hat, Inc. and others" << endl
    << "This is free software; see the source for copying conditions." << endl;
}

void
usage (systemtap_session& s, int exitcode)
{
  version ();
  clog
    << endl
    << "Usage: stap [options] FILE         Run script in file."
    << endl
    << "   or: stap [options] -            Run script on stdin."
    << endl
    << "   or: stap [options] -e SCRIPT    Run given script."
    << endl
    << endl
    << "Options:" << endl
    << "   --         no more options after this" << endl
    << "   -v         increase verbosity [" << s.verbose << "]" << endl
    << "   -h         show help" << endl
    << "   -V         show version" << endl
    << "   -k         keep temporary directory" << endl
    << "   -u         unoptimized translation" << (s.unoptimized ? " [set]" : "") << endl
    << "   -g         guru mode" << (s.guru_mode ? " [set]" : "") << endl
    << "   -b         bulk (relayfs) mode" << (s.bulk_mode ? " [set]" : "") << endl
    << "   -M         Don't merge per-cpu files for bulk (relayfs) mode" << (s.merge ? "" : " [set]") << endl
    << "   -s NUM     buffer size in megabytes, instead of "
    << s.buffer_size << endl
    << "   -p NUM     stop after pass NUM 1-5, instead of "
    << s.last_pass << endl
    << "              (parse, elaborate, translate, compile, run)" << endl
    << "   -I DIR     look in DIR for additional .stp script files";
  if (s.include_path.size() == 0)
    clog << endl;
  else
    clog << ", in addition to" << endl;
  for (unsigned i=0; i<s.include_path.size(); i++)
    clog << "              " << s.include_path[i] << endl;
  clog
    << "   -D NM=VAL  emit macro definition into generated C code" << endl
    << "   -R DIR     look in DIR for runtime, instead of" << endl
    <<      "              " << s.runtime_path << endl
    << "   -r RELEASE use kernel RELEASE, instead of "
    << s.kernel_release << endl
    << "   -m MODULE  set probe module name, instead of "
    << s.module_name << endl
    << "   -o FILE    send output to file, instead of stdout" << endl
    << "   -c CMD     start the probes, run CMD, and exit when it finishes"
    << endl
    << "   -x PID     sets target() to PID" << endl
    << "   -t         benchmarking timing information generated" << endl
    ;
  // -d: dump safety-related external references

  exit (exitcode);
}


static void
printscript(systemtap_session& s, ostream& o)
{
  if (s.embeds.size() > 0)
    o << "# global embedded code" << endl;
  for (unsigned i=0; i<s.embeds.size(); i++)
    {
      embeddedcode* ec = s.embeds[i];
      ec->print (o);
      o << endl;
    }

  if (s.globals.size() > 0)
    o << "# globals" << endl;
  for (unsigned i=0; i<s.globals.size(); i++)
    {
      vardecl* v = s.globals[i];
      v->printsig (o);
      if (s.verbose && v->init)
        {
          o << " = ";
          v->init->print(o);
        }
      o << endl;
    }

  if (s.functions.size() > 0)
    o << "# functions" << endl;
  for (unsigned i=0; i<s.functions.size(); i++)
    {
      functiondecl* f = s.functions[i];
      f->printsig (o);
      o << endl;
      if (f->locals.size() > 0)
	o << "  # locals" << endl;
      for (unsigned j=0; j<f->locals.size(); j++)
        {
	  vardecl* v = f->locals[j];
	  o << "  ";
	  v->printsig (o);
	  o << endl;
	}
      if (s.verbose)
        {
	  f->body->print (o);
	  o << endl;
	}
    }

  if (s.probes.size() > 0)
    o << "# probes" << endl;
  for (unsigned i=0; i<s.probes.size(); i++)
    {
      derived_probe* p = s.probes[i];
      p->printsig (o);
      o << endl;
      if (p->locals.size() > 0)
        o << "  # locals" << endl;
      for (unsigned j=0; j<p->locals.size(); j++)
        {
	  vardecl* v = p->locals[j];
	  o << "  ";
	  v->printsig (o);
	  o << endl;
	}
      if (s.verbose)
        {
	  p->body->print (o);
	  o << endl;
	}
    }
}

int
main (int argc, char * const argv [])
{
  string cmdline_script; // -e PROGRAM
  string script_file; // FILE
  bool have_script = false;
  bool release_changed = false;

  // Initialize defaults
  systemtap_session s;
  struct utsname buf;
  (void) uname (& buf);
  s.kernel_release = string (buf.release);
  s.architecture = string (buf.machine);
  s.verbose = 0;
  s.timing = 0;
  s.guru_mode = false;
  s.bulk_mode = false;
  s.unoptimized = false;
  s.buffer_size = 0;
  s.last_pass = 5;
  s.module_name = "stap_" + stringify(getpid());
  s.output_file = ""; // -o FILE
  s.keep_tmpdir = false;
  s.cmd = "";
  s.target_pid = 0;
  s.merge=true;
  s.perfmon=0;
  s.symtab = false;
  s.use_cache = true;

  const char* s_p = getenv ("SYSTEMTAP_TAPSET");
  if (s_p != NULL)  
  {
    s.include_path.push_back (s_p);
    s.include_path.push_back (string(s_p) + "/LKET");
  }
  else
  {
    s.include_path.push_back (string(PKGDATADIR) + "/tapset");
    s.include_path.push_back (string(PKGDATADIR) + "/tapset/LKET");
  }

  const char* s_r = getenv ("SYSTEMTAP_RUNTIME");
  if (s_r != NULL)
    s.runtime_path = s_r;
  else
    s.runtime_path = string(PKGDATADIR) + "/runtime";

  const char* s_d = getenv ("SYSTEMTAP_DIR");
  if (s_d != NULL)
    s.data_path = s_d;
  else
    s.data_path = get_home_directory() + string("/.systemtap");
  if (create_dir(s.data_path.c_str()) == 1)
    {
      const char* e = strerror (errno);
      cerr << "Warning: failed to create systemtap data directory (\""
	   << s.data_path << "\"): " << e << endl;
      cerr << "Disabling cache support." << endl;
      s.use_cache = false;
    }

  if (s.use_cache)
    {
      s.cache_path = s.data_path + "/cache";
      if (create_dir(s.cache_path.c_str()) == 1)
        {
	  const char* e = strerror (errno);
	  cerr << "Warning: failed to create cache directory (\""
	       << s.cache_path << "\"): " << e << endl;
	  cerr << "Disabling cache support." << endl;
	  s.use_cache = false;
	}
    }

  while (true)
    {
      int grc = getopt (argc, argv, "hVMvtp:I:e:o:R:r:m:kgc:x:D:bs:u");
      if (grc < 0)
        break;
      switch (grc)
        {
        case 'V':
          version ();
          exit (0);

        case 'M':
          s.merge = false;
          break;

        case 'v':
	  s.verbose ++;
	  break;

        case 't':
	  s.timing ++;
	  break;

        case 'p':
          s.last_pass = atoi (optarg);
          if (s.last_pass < 1 || s.last_pass > 5)
            {
              cerr << "Invalid pass number (should be 1-5)." << endl;
              usage (s, 1);
            }
          break;

        case 'I':
          s.include_path.push_back (string (optarg));
          break;

        case 'e':
	  if (have_script)
	    {
	      cerr << "Only one script can be given on the command line."
		   << endl;
	      usage (s, 1);
	    }
          cmdline_script = string (optarg);
          have_script = true;
          break;

        case 'o':
          s.output_file = string (optarg);
          break;

        case 'R':
          s.runtime_path = string (optarg);
          break;

        case 'm':
          s.module_name = string (optarg);
	  cerr << "Warning: using '-m' disables cache support." << endl;
	  s.use_cache = false;
          break;

        case 'r':
          s.kernel_release = string (optarg);
	  release_changed = true;
          break;

        case 'k':
          s.keep_tmpdir = true;
          break;

        case 'g':
          s.guru_mode = true;
          break;

        case 'b':
          s.bulk_mode = true;
          break;

	case 'u':
	  s.unoptimized = true;
	  break;

        case 's':
          s.buffer_size = atoi (optarg);
          if (s.buffer_size < 1 || s.buffer_size > 64)
            {
              cerr << "Invalid buffer size (should be 1-64)." << endl;
	      usage (s, 1);
            }
          break;

	case 'c':
	  s.cmd = string (optarg);
	  break;

	case 'x':
	  s.target_pid = atoi(optarg);
	  break;

	case 'D':
	  s.macros.push_back (string (optarg));
	  break;

        case 'h':
          usage (s, 0);
          break;

        default:
          usage (s, 1);
          break;
        }
    }

  if(!s.bulk_mode && !s.merge)
    {
      cerr << "-M option is valid only for bulk (relayfs) mode." <<endl;
      usage (s, 1);
    }

  if(!s.output_file.empty() && s.bulk_mode && !s.merge)
    {
      cerr << "You can't specify -M, -b and -o options together." <<endl;
      usage (s, 1);
    }

  if (s.last_pass > 4 && release_changed)
    {
      cerr << ("Warning: changing last pass to 4 since the kernel release"
	       " has changed.") << endl;
      s.last_pass = 4;
    }

  for (int i = optind; i < argc; i++)
    {
      if (! have_script)
        {
          script_file = string (argv[i]);
          have_script = true;
        }
      else
        s.args.push_back (string (argv[i]));
    }

  // need a user file
  if (! have_script)
    {
      cerr << "A script must be specified." << endl;
      usage(s, 1);
    }

  int rc = 0;

  // override PATH and LC_ALL
  const char *path = "/bin:/sbin:/usr/bin:/usr/sbin";
  rc = setenv("PATH", path, 1) || setenv("LC_ALL", "C", 1);
  if (rc)
    {
      const char* e = strerror (errno);
      cerr << "setenv (\"PATH=" << path << "\" + \"LC_ALL=C\"): "
           << e << endl;
    }

  s.kernel_base_release.assign(s.kernel_release, 0, s.kernel_release.find('-'));

  // arguments parsed; get down to business
  if (s.verbose > 1)
    version ();

  // Create a temporary directory to build within.
  // Be careful with this, as "s.tmpdir" is "rm -rf"'d at the end.
  {
    const char* tmpdir_env = getenv("TMPDIR");
    if (! tmpdir_env)
      tmpdir_env = "/tmp";
    
    string stapdir = "/stapXXXXXX";
    string tmpdirt = tmpdir_env + stapdir;
    const char* tmpdir = mkdtemp((char *)tmpdirt.c_str());
    if (! tmpdir)
      {
        const char* e = strerror (errno);
        cerr << "ERROR: cannot create temporary directory (\"" << tmpdirt << "\"): " << e << endl;
        exit (1); // die
      }
    else
      s.tmpdir = tmpdir;

    if (s.verbose>1)
      clog << "Created temporary directory \"" << s.tmpdir << "\"" << endl;
  }

  // Create the name of the C source file within the temporary
  // directory.
  s.translated_source = string(s.tmpdir) + "/" + s.module_name + ".c";

  struct tms tms_before;
  times (& tms_before);
  struct timeval tv_before;
  gettimeofday (&tv_before, NULL);

  // PASS 1a: PARSING USER SCRIPT

  struct stat user_file_stat;
  int user_file_stat_rc = -1;

  if (script_file == "-")
    {
      s.user_file = parser::parse (s, cin, s.guru_mode);
      user_file_stat_rc = fstat (STDIN_FILENO, & user_file_stat);
    }
  else if (script_file != "")
    {
      s.user_file = parser::parse (s, script_file, s.guru_mode);
      user_file_stat_rc = stat (script_file.c_str(), & user_file_stat);
    }
  else
    {
      istringstream ii (cmdline_script);
      s.user_file = parser::parse (s, ii, s.guru_mode);
    }
  if (s.user_file == 0)
    // syntax errors already printed
    rc ++;

  // Construct arch / kernel-versioning search path
  vector<string> version_suffixes;
  string kvr = s.kernel_release;
  const string& arch = s.architecture;
  // add full kernel-version-release (2.6.NN-FOOBAR) + arch
  version_suffixes.push_back ("/" + kvr + "/" + arch);
  version_suffixes.push_back ("/" + kvr);
  // add kernel version (2.6.NN) + arch
  if (kvr != s.kernel_base_release) {
    kvr = s.kernel_base_release;
    version_suffixes.push_back ("/" + kvr + "/" + arch);
    version_suffixes.push_back ("/" + kvr);
  }
  // add kernel family (2.6) + arch
  string::size_type dot1_index = kvr.find ('.');
  string::size_type dot2_index = kvr.rfind ('.');
  while (dot2_index > dot1_index && dot2_index != string::npos) {
    kvr.erase(dot2_index);
    version_suffixes.push_back ("/" + kvr + "/" + arch);
    version_suffixes.push_back ("/" + kvr);
    dot2_index = kvr.rfind ('.');
  }
  // add architecture search path
  version_suffixes.push_back("/" + arch);
  // add empty string as last element
  version_suffixes.push_back ("");

  // PASS 1b: PARSING LIBRARY SCRIPTS
  for (unsigned i=0; i<s.include_path.size(); i++)
    {
      // now iterate upon it
      for (unsigned k=0; k<version_suffixes.size(); k++)
        {
          glob_t globbuf;
          string dir = s.include_path[i] + version_suffixes[k] + "/*.stp";
          int r = glob(dir.c_str (), 0, NULL, & globbuf);
          if (r == GLOB_NOSPACE || r == GLOB_ABORTED)
            rc ++;
          // GLOB_NOMATCH is acceptable

          if (s.verbose>1)
            clog << "Searched '" << dir << "', "
                 << "match count " << globbuf.gl_pathc << endl;

          for (unsigned j=0; j<globbuf.gl_pathc; j++)
            {
              // privilege only for /usr/share/systemtap?
              
              stapfile* f = parser::parse (s, globbuf.gl_pathv[j], true);
              if (f == 0)
                rc ++;
              else
                s.library_files.push_back (f);

              struct stat tapset_file_stat;
              int stat_rc = stat (globbuf.gl_pathv[j], & tapset_file_stat);
              if (stat_rc == 0 && user_file_stat_rc == 0 &&
                  user_file_stat.st_dev == tapset_file_stat.st_dev &&
                  user_file_stat.st_ino == tapset_file_stat.st_ino)
                {
                  clog << "usage error: tapset file '" << globbuf.gl_pathv[j]
                       << "' cannot be run directly as a session script." << endl;
                  rc ++;
                }

            }

          globfree (& globbuf);
        }
    }

  if (rc == 0 && s.last_pass == 1)
    {
      cout << "# parse tree dump" << endl;
      s.user_file->print (cout);
      cout << endl;
      if (s.verbose)
        for (unsigned i=0; i<s.library_files.size(); i++)
          {
            s.library_files[i]->print (cout);
            cout << endl;
          }
    }

  struct tms tms_after;
  times (& tms_after);
  unsigned _sc_clk_tck = sysconf (_SC_CLK_TCK);
  struct timeval tv_after;
  gettimeofday (&tv_after, NULL);

#define TIMESPRINT \
           (tms_after.tms_cutime + tms_after.tms_utime \
            - tms_before.tms_cutime - tms_before.tms_utime) * 1000 / (_sc_clk_tck) << "usr/" \
        << (tms_after.tms_cstime + tms_after.tms_stime \
            - tms_before.tms_cstime - tms_before.tms_stime) * 1000 / (_sc_clk_tck) << "sys/" \
        << ((tv_after.tv_sec - tv_before.tv_sec) * 1000 + \
            ((long)tv_after.tv_usec - (long)tv_before.tv_usec) / 1000) << "real ms."

  // syntax errors, if any, are already printed
  if (s.verbose)
    {
      clog << "Pass 1: parsed user script and "
           << s.library_files.size()
           << " library script(s) in "
           << TIMESPRINT
           << endl;
    }

  if (rc)
    cerr << "Pass 1: parse failed.  "
         << "Try again with more '-v' (verbose) options."
         << endl;

  if (rc || s.last_pass == 1) goto cleanup;

  times (& tms_before);
  gettimeofday (&tv_before, NULL);

  // PASS 2: ELABORATION
  rc = semantic_pass (s);

  if (rc == 0 && s.last_pass == 2)
    printscript(s, cout);

  times (& tms_after);
  gettimeofday (&tv_after, NULL);

  if (s.verbose) clog << "Pass 2: analyzed script: "
                      << s.probes.size() << " probe(s), "
                      << s.functions.size() << " function(s), "
                      << s.embeds.size() << " embed(s), "
                      << s.globals.size() << " global(s) in "
                      << TIMESPRINT
                      << endl;

  if (rc)
    cerr << "Pass 2: analysis failed.  "
         << "Try again with more '-v' (verbose) options."
         << endl;
  // Generate hash.  There isn't any point in generating the hash
  // if last_pass is 2, since we'll quit before using it.
  else if (s.last_pass != 2 && s.use_cache)
    {
      ostringstream o;
      unsigned saved_verbose;

      {
        // Make sure we're in verbose mode, so that printscript()
        // will output function/probe bodies.
        saved_verbose = s.verbose;
        s.verbose = 3;
        printscript(s, o);  // Print script to 'o'
        s.verbose = saved_verbose;
      }

      // Generate hash
      find_hash (s, o.str());

      // See if we can use cached source/module.
      if (get_from_cache(s))
        {
	  // If our last pass isn't 5, we're done (since passes 3 and
	  // 4 just generate what we just pulled out of the cache).
	  if (s.last_pass < 5) goto cleanup;

	  // Short-circuit to pass 5.
	  goto pass_5;
	}
    }

  if (rc || s.last_pass == 2) goto cleanup;

  // PASS 3: TRANSLATION

  times (& tms_before);
  gettimeofday (&tv_before, NULL);

  rc = translate_pass (s);

  if (rc == 0 && s.last_pass == 3)
    {
      ifstream i (s.translated_source.c_str());
      cout << i.rdbuf();
    }

  times (& tms_after);
  gettimeofday (&tv_after, NULL);

  if (s.verbose) clog << "Pass 3: translated to C into \""
                      << s.translated_source
                      << "\" in "
                      << TIMESPRINT
                      << endl;

  if (rc)
    cerr << "Pass 3: translation failed.  "
         << "Try again with more '-v' (verbose) options."
         << endl;

  if (rc || s.last_pass == 3) goto cleanup;

  // PASS 4: COMPILATION
  times (& tms_before);
  gettimeofday (&tv_before, NULL);
  rc = compile_pass (s);

  if (rc == 0 && s.last_pass == 4)
    cout << s.hash_path << endl;

  times (& tms_after);
  gettimeofday (&tv_after, NULL);

  if (s.verbose) clog << "Pass 4: compiled C into \""
                      << s.module_name << ".ko"
                      << "\" in "
                      << TIMESPRINT
                      << endl;

  if (rc)
    cerr << "Pass 4: compilation failed.  "
         << "Try again with more '-v' (verbose) options."
         << endl;
  else if (s.use_cache)
    {
      // Update cache.
      add_to_cache(s);
    }

  if (rc || s.last_pass == 4) goto cleanup;


  // PASS 5: RUN
pass_5:
  times (& tms_before);
  gettimeofday (&tv_before, NULL);
  // NB: this message is a judgement call.  The other passes don't emit
  // a "hello, I'm starting" message, but then the others aren't interactive
  // and don't take an indefinite amount of time.
  if (s.verbose) clog << "Pass 5: starting run." << endl;
  rc = run_pass (s);
  times (& tms_after);
  gettimeofday (&tv_after, NULL);
  if (s.verbose) clog << "Pass 5: run completed in "
                      << TIMESPRINT
                      << endl;

  if (rc)
    cerr << "Pass 5: run failed.  "
         << "Try again with more '-v' (verbose) options."
         << endl;

  // if (rc) goto cleanup;

 cleanup:
  // Clean up temporary directory.  Obviously, be careful with this.
  if (s.tmpdir == "")
    ; // do nothing
  else
    {
      if (s.keep_tmpdir)
        clog << "Keeping temporary directory \"" << s.tmpdir << "\"" << endl;
      else
        {
          string cleanupcmd = "rm -rf ";
          cleanupcmd += s.tmpdir;
          if (s.verbose>1) clog << "Running " << cleanupcmd << endl;
	  int status = system (cleanupcmd.c_str());
	  if (status != 0 && s.verbose>1)
	    clog << "Cleanup command failed, status: " << status << endl;
        }
    }

  return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}
