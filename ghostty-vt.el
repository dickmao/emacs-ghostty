;;; ghostty-vt.el --- Ghostty VT terminal emulator  -*- lexical-binding: t -*-

(require 'ghostty-vt-module)

(defgroup ghostty-vt nil
  "Ghostty VT terminal emulator."
  :group 'terminals)

(defcustom ghostty-vt-shell (or (getenv "SHELL") "/bin/sh")
  "Shell program for ghostty-vt."
  :type 'string
  :group 'ghostty-vt)

(defcustom ghostty-vt-min-window-width 80
  "Minimum window width."
  :type 'integer
  :group 'ghostty-vt)

(defcustom ghostty-vt-max-scrollback 1000
  "Maximum scrollback lines."
  :type 'integer
  :group 'ghostty-vt)

(defvar-local ghostty-vt--term nil)
(defvar-local ghostty-vt--process nil)

(defun ghostty-vt--redraw ()
  (let ((inhibit-read-only t))
    (when (ghostty-vt--render ghostty-vt--term)
      (if-let ((pos (ghostty-vt--cursor-pos ghostty-vt--term)))
        (progn
          (setq cursor-type t)
          (goto-char (point-min))
          (forward-line (1- (car pos)))
          (forward-char (1- (cdr pos))))
        (setq cursor-type nil)))))

(defun ghostty-vt--filter (proc output)
  (when-let ((buf (process-buffer proc)))
    (with-current-buffer buf
      (ghostty-vt--write-input ghostty-vt--term output)
      (ghostty-vt--redraw))))

(defun ghostty-vt--self-insert ()
  (interactive)
  (let* ((keys (this-command-keys-vector))
         (event (aref keys (1- (length keys))))
         (mods (event-modifiers event))
         (basic (event-basic-type event))
         (keystr (if (characterp basic)
                     (string basic)
                   (format "<%s>" basic)))
         (encoded (ghostty-vt--send-key ghostty-vt--term keystr
                                        (and (memq 'shift mods) t)
                                        (and (memq 'meta mods) t)
                                        (and (memq 'control mods) t))))
    (when (and encoded (> (length encoded) 0))
      (process-send-string ghostty-vt--process encoded))))

(defun ghostty-vt--adjust-window-size (process windows)
  (let* ((size (funcall window-adjust-process-window-size-function process windows))
         (cols (max (car size) ghostty-vt-min-window-width))
         (rows (cdr size)))
    (when (and (> cols 0) (> rows 0))
      (ghostty-vt--resize ghostty-vt--term rows cols
                          (frame-char-width) (frame-char-height))
      (cons cols rows))))

(defvar ghostty-vt-mode-map
  (let ((map (make-sparse-keymap)))
    (define-key map [t] #'ghostty-vt--self-insert)
    map))

(define-derived-mode ghostty-vt-mode fundamental-mode "GhosttyVT"
  "Major mode for ghostty-vt."
  (setq-local
   buffer-read-only t
   buffer-undo-list t
   font-lock-defaults '(nil t) ;defang font-lock
   scroll-conservatively 101 ;>100 never recenter point
   scroll-margin 0
   hscroll-margin 0
   hscroll-step 1
   ghostty-vt--term (ghostty-vt--new
		     (window-body-height)
		     (max (window-max-chars-per-line)
			  ghostty-vt-min-window-width)
		     ghostty-vt-max-scrollback)
   ghostty-vt--process
   (make-process
    :name "ghostty-vt"
    :buffer (current-buffer)
    :command
    `("/bin/sh" "-c"
      ,(format
	"stty -nl sane %s erase ^? rows %d columns %d >/dev/null && exec %s"
	;; https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=220009
	(if (eq system-type 'berkeley-unix) "" "iutf8")
	(window-body-height)
	(max (window-max-chars-per-line) ghostty-vt-min-window-width)
	ghostty-vt-shell))
    :coding 'no-conversion
    :connection-type 'pty
    :file-handler t
    :filter #'ghostty-vt--filter
    :sentinel (lambda (proc _msg)
                (when (buffer-live-p (process-buffer proc))
                  (kill-buffer (process-buffer proc))))))
  (require 'hl-line)
  (require 'display-line-numbers)
  (dolist (mode '(display-line-numbers-mode hl-line-mode))
    (let* ((mode-hook (intern-soft (concat (symbol-name mode) "-hook")))
	   (mode-hook-value (ignore-errors (symbol-value mode-hook)))
	   (negatory (lambda ()
		       (cl-letf (((symbol-value mode-hook) mode-hook-value))
			 (funcall mode -1)
			 (user-error "No dude")))))
      (funcall mode -1)
      (add-hook mode-hook negatory nil :local)))
  (add-hook 'change-major-mode-hook
	    (lambda () (user-error "No dude"))
	    nil :local)
  (process-put ghostty-vt--process 'adjust-window-size-function
	       #'ghostty-vt--adjust-window-size)

  ;; Set the truncation slot for `buffer-display-table' to the ASCII code for a
  ;; space character (32) to make the vterm buffer display a space instead of
  ;; the default truncation character ($) when a line is truncated.
  (let ((display-table (or buffer-display-table (make-display-table))))
    (set-display-table-slot display-table 'truncation 32)
    (setq buffer-display-table display-table)))

;;;###autoload
(defun ghostty-vt (&optional arg)
  "Open a ghostty-vt terminal buffer."
  (interactive "P")
  (pop-to-buffer-same-window
   (with-current-buffer (if arg (generate-new-buffer "*ghostty-vt*")
			  (get-buffer-create "*ghostty-vt*"))
     (unless (derived-mode-p 'ghostty-vt-mode)
       (ghostty-vt-mode))
     (current-buffer))))

(provide 'ghostty-vt)
;;; ghostty-vt.el ends here
