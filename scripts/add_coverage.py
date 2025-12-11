# add_coverage.py
Import("env")

# Force the --coverage flag into the linker
env.Append(LINKFLAGS=["--coverage"])