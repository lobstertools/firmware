Import("env")
import os
import sys
import subprocess

def install_clang_format():
    """Installs clang-format via pip if not present."""
    print("Checking for clang-format...")
    try:
        subprocess.check_call([sys.executable, "-m", "pip", "install", "clang-format"])
    except subprocess.CalledProcessError:
        print("Error: Failed to install clang-format.")
        env.Exit(1)

def get_project_files(env):
    """Scans src and include folders for C/C++ files."""
    # Define directories to check
    check_folders = [env.get("PROJECT_SRC_DIR"), env.get("PROJECT_INCLUDE_DIR")]
    
    files_to_process = []
    # Extensions to look for
    valid_extensions = ('.c','.cpp', '.h', '.hpp', '.cc', '.cxx', '.hxx','.hh')

    for folder in check_folders:
        if not folder or not os.path.exists(folder):
            continue

        for root, dirs, files in os.walk(folder):
            for file in files:
                if file.endswith(valid_extensions):
                    files_to_process.append(os.path.join(root, file))
    
    return files_to_process

def format_callback(*args, **kwargs):
    """Target: Formats source code in-place."""
    install_clang_format()
    files = get_project_files(env)
    
    if not files:
        print("No source files found to format.")
        return

    print(f"Formatting {len(files)} files...")
    
    for file in files:
        # -i = in-place edit
        # We quote the filename to handle spaces safely
        env.Execute(f'clang-format -i "{file}"')
    
    print("Done!")

def verify_callback(*args, **kwargs):
    """Target: Verifies formatting without modifying files."""
    install_clang_format()
    files = get_project_files(env)

    if not files:
        print("No source files found to verify.")
        return

    print(f"Verifying {len(files)} files...")
    
    error_count = 0
    
    for file in files:
        # -n = dry-run (don't edit)
        # --Werror = return non-zero exit code if formatting is needed
        cmd = ["clang-format", "-n", "--Werror", file]
        
        # Run subprocess directly to capture exit code without crashing immediately
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            print(f"❌ Formatting Error: {file}")
            # Print the specific violation if you want verbose output:
            # print(result.stderr) 
            error_count += 1
        else:
            print(f"✅ Clean: {os.path.basename(file)}")

    if error_count > 0:
        print(f"\nFAILED: {error_count} file(s) need formatting.")
        print("Run 'pio run -t format' to fix them.")
        env.Exit(1) # Fail the build
    else:
        print("\nSUCCESS: All files are formatted correctly.")

# Register the "format" target
env.AddCustomTarget(
    "format",
    None,
    format_callback,
    title="Format Code",
    description="Formats source code in-place using clang-format"
)

# Register the "verify" target
env.AddCustomTarget(
    "verify",
    None,
    verify_callback,
    title="Verify Formatting",
    description="Checks source code for formatting errors"
)