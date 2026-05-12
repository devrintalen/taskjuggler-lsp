project = "taskjuggler-lsp"
author = "Devrin Talen"
copyright = "2026, Devrin Talen"
release = "0.5.2"

extensions = ["breathe"]
exclude_patterns = ["_build", "_doxygen", "Thumbs.db", ".DS_Store"]

html_theme = "sphinx_rtd_theme"

# Breathe pulls API documentation out of Doxygen's XML output. The XML
# tree is produced by `doxygen Doxyfile` (run from the repo root) into
# doc/_doxygen/xml/, and on Read the Docs the pre_build job in
# .readthedocs.yaml runs the same command before Sphinx starts.
breathe_projects = {"taskjuggler-lsp": "_doxygen/xml"}
breathe_default_project = "taskjuggler-lsp"
breathe_default_members = ("members",)
breathe_domain_by_extension = {"h": "c", "c": "c"}
