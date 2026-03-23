;;; ghostty-vt.el --- Ghostty VT terminal emulator  -*- lexical-binding: t -*-

(require 'ghostty-vt-module)
(require 'ansi-color)

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
    (erase-buffer)
    (insert (ghostty-vt--render ghostty-vt--term))
    (ansi-color-apply-on-region (point-min) (point-max))
    (when-let ((pos (ghostty-vt--cursor-pos ghostty-vt--term)))
      (goto-char (point-min))
      (forward-line (1- (car pos)))
      (forward-char (1- (cdr pos))))))

(defun ghostty-vt--process-filter (proc output)
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
      (ghostty-vt--resize ghostty-vt--term rows cols)
      (cons cols rows))))

(defvar ghostty-vt-mode-map
  (let ((map (make-sparse-keymap)))
    (define-key map [t] #'ghostty-vt--self-insert)
    map))

(define-derived-mode ghostty-vt-mode fundamental-mode "GhosttyVT"
  "Major mode for ghostty-vt."
  (setq buffer-read-only t)
  (buffer-disable-undo)
  (let ((inhibit-eol-conversion nil)
        (coding-system-for-read 'binary)
        (process-adaptive-read-buffering nil)
        (width (max (- (window-max-chars-per-line) (vterm--get-margin-width))
                    ghostty-vt-min-window-width)))
    (setq ghostty-vt--term (ghostty-vt--new (window-body-height)
					    width ghostty-vt-max-scrollback))
    (setq-local buffer-read-only t
		scroll-conservatively 101 ;>100 never recenter point
		scroll-margin 0
		hscroll-margin 0
		hscroll-step 1
		truncate-lines t)

    ;; font-lock-mode remains on but defanged
    (setq-local font-lock-defaults '(nil t))

    (setq ghostty-vt--process
          (make-process
           :name "ghostty-vt"
           :buffer (current-buffer)
           :command
           `("/bin/sh" "-c"
             ,(format
               "stty -nl sane %s erase ^? rows %d columns %d >/dev/null && exec %s"
               ;; Some stty implementations (i.e. that of *BSD) do not
               ;; support the iutf8 option.  to handle that, we run some
               ;; heuristics to work out if the system supports that
               ;; option and set the arg string accordingly. This is a
               ;; gross hack but FreeBSD doesn't seem to want to fix it.
               ;;
               ;; See: https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=220009
               (if (eq system-type 'berkeley-unix) "" "iutf8")
               (window-body-height)
               width (ghostty-vt--get-shell)))
           ;; :coding 'no-conversion
           :connection-type 'pty
           :file-handler t
           :filter #'ghostty-vt--filter
           ;; The sentinel is needed if there are exit functions or if
           ;; ghostty-vt-kill-buffer-on-exit is set to t.  In this latter case,
           ;; ghostty-vt--sentinel will kill the buffer
           :sentinel (when (or ghostty-vt-exit-functions
                               ghostty-vt-kill-buffer-on-exit)
                       #'ghostty-vt--sentinel))))
  (let* ((buf (get-buffer-create "*ghostty-vt*"))
         (win (or (get-buffer-window buf) (selected-window)))
         (rows (window-body-height win))
         (cols (window-body-width win)))
    (with-current-buffer buf
      (ghostty-vt-mode)
      (setq ghostty-vt--term
            (ghostty-vt--new rows cols ghostty-vt-max-scrollback))
      (setq ghostty-vt--process
            (start-process "ghostty-vt" buf
                           "/bin/sh" "-c"
                           (format "stty sane erase '^?' rows %d columns %d && exec %s"
                                   rows cols ghostty-vt-shell)))

      (add-hook 'change-major-mode-hook
		(lambda () (interactive)
		  (user-error "You cannot change major mode in ghostty-vt buffers")) nil t)

      (process-put ghostty-vt--process 'adjust-window-size-function
                   #'ghostty-vt--adjust-window-size)
      (set-process-coding-system ghostty-vt--process 'no-conversion 'no-conversion)
      (set-process-filter ghostty-vt--process #'ghostty-vt--process-filter)
      (set-process-sentinel ghostty-vt--process
                            (lambda (proc _msg)
                              (when (buffer-live-p (process-buffer proc))
                                (kill-buffer (process-buffer proc))))))
    (switch-to-buffer buf)))

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
