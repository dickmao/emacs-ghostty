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

(defun ghostty-vt--window-size-change (frame)
  (walk-windows
   (lambda (win)
     (with-current-buffer (window-buffer win)
       (when (and (eq major-mode 'ghostty-vt-mode)
                  ghostty-vt--term ghostty-vt--process)
         (let ((rows (window-body-height win))
               (cols (window-body-width win)))
           (ghostty-vt--resize ghostty-vt--term rows cols)
           (set-process-window-size ghostty-vt--process rows cols)))))
   nil t frame))

(defvar ghostty-vt-mode-map
  (let ((map (make-sparse-keymap)))
    (define-key map [t] #'ghostty-vt--self-insert)
    map))

(define-derived-mode ghostty-vt-mode fundamental-mode "GhosttyVT"
  "Major mode for ghostty-vt terminal emulator."
  (setq buffer-read-only nil)
  (buffer-disable-undo)
  (add-hook 'window-size-change-functions #'ghostty-vt--window-size-change nil t))

;;;###autoload
(defun ghostty-vt ()
  "Open a ghostty-vt terminal buffer."
  (interactive)
  (let* ((buf (get-buffer-create "*ghostty-vt*"))
         (win (or (get-buffer-window buf) (selected-window)))
         (rows (window-body-height win))
         (cols (window-body-width win)))
    (with-current-buffer buf
      (ghostty-vt-mode)
      (setq ghostty-vt--term
            (ghostty-vt--new rows cols ghostty-vt-max-scrollback))
      (let ((process-connection-type t)
            (process-environment
             (cons "TERM=xterm-256color" process-environment)))
        (setq ghostty-vt--process
              (start-process "ghostty-vt" buf
                             "/bin/sh" "-c"
                             (format "stty sane erase '^?' rows %d columns %d && exec %s"
                                     rows cols ghostty-vt-shell))))
      (set-process-coding-system ghostty-vt--process 'no-conversion 'no-conversion)
      (set-process-filter ghostty-vt--process #'ghostty-vt--process-filter)
      (set-process-sentinel ghostty-vt--process
                            (lambda (proc _msg)
                              (when (buffer-live-p (process-buffer proc))
                                (kill-buffer (process-buffer proc))))))
    (switch-to-buffer buf)))

(provide 'ghostty-vt)
;;; ghostty-vt.el ends here
