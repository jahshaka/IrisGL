// THE LANGUAGE TABLE (SCRIPT-IN-ENGINE-1, the lead's S5) - ONE LINE A LANGUAGE: the file extension of its scripts and
// its binding's maker, `std::unique_ptr<Host::Language> <maker>(Host &)`, defined in the binding's ONE file under
// src/script/ (the only file that names its engine). Adding a language = that one file and one line here (+ its vendored
// tree and the build's source list); source.script_neutral holds the table and the files to each other. Read twice by
// macro (Language.h declares the makers; Host's constructor adds each line): no other file names a language.
JAH_SCRIPT_LANGUAGE(".js", makeJavaScript)
