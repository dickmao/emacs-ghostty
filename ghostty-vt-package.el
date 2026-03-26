;;; ghostty-vt-package.el --- because package.el sucks ass  -*- lexical-binding:t -*-

(require 'package)
(require 'project)

(defsubst ghostty-vt-package-where ()
  (directory-file-name (expand-file-name (project-root (project-current)))))

(defsubst ghostty-vt-package-desc ()
  (with-temp-buffer
    (insert-file-contents
     (expand-file-name "ghostty-vt.el" (ghostty-vt-package-where)))
    (package-buffer-info)))

(defun ghostty-vt-package-name ()
  (concat "ghostty-vt-" (package-version-join
			      (package-desc-version
			       (ghostty-vt-package-desc)))))

(defun ghostty-vt-package-inception ()
  "To get a -pkg.el file, you need to run `package-unpack'.
To run `package-unpack', you need a -pkg.el."
  (let ((pkg-desc (ghostty-vt-package-desc))
	(pkg-dir (expand-file-name (ghostty-vt-package-name)
				   (ghostty-vt-package-where))))
    (ignore-errors (delete-directory pkg-dir t))
    (make-directory pkg-dir t)
    (copy-file (expand-file-name "ghostty-vt.el" (ghostty-vt-package-where))
	       (expand-file-name "ghostty-vt.el" pkg-dir))
    (package--make-autoloads-and-stuff pkg-desc pkg-dir)))
