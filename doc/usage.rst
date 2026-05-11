Usage
=====

Configure your editor to launch ``taskjuggler-lsp`` as the language
server for ``.tjp`` and ``.tji`` files. The server communicates over
standard input / output using the LSP JSON-RPC protocol.

Emacs (lsp-mode)
----------------

.. code-block:: emacs-lisp

   (use-package lsp-mode
     :init
     (setq lsp-keymap-prefix "C-c l")
     (setq lsp-semantic-tokens-enable t)
     (setq lsp-log-io t)
     :hook ((taskjuggler-mode . lsp))
     :commands lsp
     :config
     (setq lsp-completion-no-cache t))

   (use-package lsp-ui
     :hook (lsp-mode . lsp-ui-mode)
     :config
     (setq lsp-ui-doc-show-with-cursor t))

   (with-eval-after-load 'lsp-mode
     (lsp-register-client
      (make-lsp-client
       :new-connection (lsp-stdio-connection
                        "/path/to/taskjuggler-lsp/taskjuggler-lsp")
       :major-modes '(taskjuggler-mode)
       :server-id 'taskjuggler-lsp)))

``lsp-completion-no-cache t`` is recommended so that dependency
completions refresh correctly when the user types additional ``!``
characters. Each extra ``!`` widens the reference scope rather than
narrowing the list, so lsp-mode's same-session cache would otherwise
keep the previous (narrower) result set and hide the newly relevant
items.

This initialization code depends on `taskjuggler-mode.el`_, an Emacs
major mode for TaskJuggler.

.. _taskjuggler-mode.el: https://github.com/devrintalen/taskjuggler-mode.el
