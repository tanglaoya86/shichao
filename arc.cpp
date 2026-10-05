// ============================================================================
// Arc 语言 v4.2 解释器（单文件）—— 修正版
// ============================================================================
// 结构：
//   P1  头文件、错误、类型、Token、关键字
//   P2  词法分析器 Lexer
//   P3  预处理器 Preprocessor
//   P4  抽象语法树 AST
//   P5  语法解析器 Parser（骨架、顶层、类型、声明）
//   P6  语法解析器 Parser（语句、模式）
//   P7  语法解析器 Parser（表达式）
//   P8  运行时 Value / Env / Object / Array / Map / Opt / Rlt
//   P9  解释器（表达式求值）
//   P10 解释器（语句执行 + 协程状态机）
//   P11 Interpreter::run() + main 驱动
// ============================================================================
#include <variant>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace arc {

// ============================================================================
// P1-1  错误类型
// ============================================================================
struct ArcError : std::runtime_error {
    int line = 0;
    int col  = 0;

    explicit ArcError(std::string msg, int ln = 0, int cl = 0)
        : std::runtime_error(std::move(msg)), line(ln), col(cl) {}

    static ArcError at(int ln, int cl, const std::string& msg) {
        std::string full = "line " + std::to_string(ln);
        if (cl > 0) full += ", col " + std::to_string(cl);
        full += ": " + msg;
        return ArcError(std::move(full), ln, cl);
    }
};

// ============================================================================
// P1-2  类型表示
// ============================================================================
struct Type;
using TypePtr = std::shared_ptr<Type>;

struct Type {
    enum K {
        Void, Nil, Int, Flt, Chr, Bol, Str, Cod,
        Arr, Map, Ptr, OwnPtr, Opt, Sig, Rng, Slc, Tup, Tck, Rlt, Fun,
        Named,
    } kind = Void;

    std::string          name;
    std::vector<TypePtr> args;
    int64_t              arraySize = 0;

    Type() = default;
    explicit Type(K k) : kind(k) {}

    static TypePtr base(K k) { return std::make_shared<Type>(k); }

    static TypePtr named(const std::string& n) {
        auto t = std::make_shared<Type>(Named);
        t->name = n;
        return t;
    }
    static TypePtr makeArr(TypePtr elem, int64_t n) {
        auto t = std::make_shared<Type>(Arr);
        t->args.push_back(std::move(elem));
        t->arraySize = n;
        return t;
    }
    static TypePtr makePtr(TypePtr p) {
        auto t = std::make_shared<Type>(Ptr);
        t->args.push_back(std::move(p));
        return t;
    }
    static TypePtr makeOwnPtr(TypePtr p) {
        auto t = std::make_shared<Type>(OwnPtr);
        t->args.push_back(std::move(p));
        return t;
    }
    static TypePtr makeOpt(TypePtr inner) {
        auto t = std::make_shared<Type>(Opt);
        t->args.push_back(std::move(inner));
        return t;
    }
    static TypePtr makeRng(TypePtr inner) {
        auto t = std::make_shared<Type>(Rng);
        t->args.push_back(std::move(inner));
        return t;
    }
    static TypePtr makeSlc(TypePtr inner) {
        auto t = std::make_shared<Type>(Slc);
        t->args.push_back(std::move(inner));
        return t;
    }
    static TypePtr makeTck(TypePtr inner) {
        auto t = std::make_shared<Type>(Tck);
        t->args.push_back(std::move(inner));
        return t;
    }
    static TypePtr makeMap(TypePtr k, TypePtr v) {
        auto t = std::make_shared<Type>(Map);
        t->args.push_back(std::move(k));
        t->args.push_back(std::move(v));
        return t;
    }
    static TypePtr makeRlt(TypePtr ok, TypePtr err) {
        auto t = std::make_shared<Type>(Rlt);
        t->args.push_back(std::move(ok));
        t->args.push_back(std::move(err));
        return t;
    }
    static TypePtr makeTup(std::vector<TypePtr> elems) {
        auto t = std::make_shared<Type>(Tup);
        t->args = std::move(elems);
        return t;
    }
    static TypePtr makeFun(std::vector<TypePtr> params, TypePtr ret) {
        auto t = std::make_shared<Type>(Fun);
        t->args = std::move(params);
        t->args.push_back(std::move(ret));
        return t;
    }
    static TypePtr makeSig(const std::string& n) {
        auto t = std::make_shared<Type>(Sig);
        t->name = n;
        return t;
    }

    static TypePtr fromBaseName(const std::string& n) {
        if (n == "void") return base(Void);
        if (n == "nil")  return base(Nil);
        if (n == "int")  return base(Int);
        if (n == "flt")  return base(Flt);
        if (n == "chr")  return base(Chr);
        if (n == "bol")  return base(Bol);
        if (n == "str")  return base(Str);
        if (n == "cod")  return base(Cod);
        return named(n);
    }

    bool isNil()      const { return kind == Nil; }
    bool isInt()      const { return kind == Int; }
    bool isFlt()      const { return kind == Flt; }
    bool isChr()      const { return kind == Chr; }
    bool isBol()      const { return kind == Bol; }
    bool isStr()      const { return kind == Str; }
    bool isCod()      const { return kind == Cod; }
    bool isIntegral() const { return kind == Int || kind == Chr || kind == Bol; }
    bool isNumeric()  const { return isIntegral() || kind == Flt; }
    bool isPtr()      const { return kind == Ptr || kind == OwnPtr || kind == Cod; }
    bool isScalar()   const {
        switch (kind) {
            case Int: case Flt: case Chr: case Bol: case Str:
            case Cod: case Ptr: case OwnPtr: case Nil:
            case Named: case Sig:
                return true;
            default:
                return false;
        }
    }

    int64_t size() const {
        switch (kind) {
            case Void:   return 0;
            case Nil:    return 8;
            case Int:    return 8;
            case Flt:    return 8;
            case Chr:    return 1;
            case Bol:    return 1;
            case Str:    return 16;
            case Cod:    return 8;
            case Ptr:    return 8;
            case OwnPtr: return 8;
            case Rng:    return 16;
            case Slc:    return 16;
            case Opt:    return args.empty() ? 16 : args[0]->size() + 1;
            case Rlt:    return 16;
            case Tck:    return 8;
            case Arr:    return arraySize * (args.empty() ? 0 : args[0]->size());
            case Tup: {
                int64_t s = 0;
                for (auto& a : args) s += a->size();
                return s;
            }
            case Fun: case Sig: return 8;
            case Map: case Named: return 8;
        }
        return 8;
    }

    int64_t align() const {
        switch (kind) {
            case Chr: case Bol: return 1;
            case Int: case Flt: case Str: case Cod:
            case Ptr: case OwnPtr: case Fun: case Sig: case Tck:
                return 8;
            case Arr:
                return args.empty() ? 1 : args[0]->align();
            case Tup: {
                int64_t a = 1;
                for (auto& t : args) a = std::max(a, t->align());
                return a;
            }
            default: return 8;
        }
    }

    std::string toString() const {
        switch (kind) {
            case Void: return "void";
            case Nil:  return "nil";
            case Int:  return "int";
            case Flt:  return "flt";
            case Chr:  return "chr";
            case Bol:  return "bol";
            case Str:  return "str";
            case Cod:  return "cod";
            case Named: return name;
            case Sig:   return "sig " + name;
            case Ptr:
                return "*" + (args.empty() ? std::string("?") : args[0]->toString());
            case OwnPtr:
                return "^" + (args.empty() ? std::string("?") : args[0]->toString());
            case Opt:
                return "opt<" + (args.empty() ? std::string("?") : args[0]->toString()) + ">";
            case Rng:
                return "rng<" + (args.empty() ? std::string("?") : args[0]->toString()) + ">";
            case Slc:
                return "slc<" + (args.empty() ? std::string("?") : args[0]->toString()) + ">";
            case Tck:
                return "tck<" + (args.empty() ? std::string("?") : args[0]->toString()) + ">";
            case Arr: {
                std::string e = args.empty() ? "?" : args[0]->toString();
                return "arr<" + e + ", " + std::to_string(arraySize) + ">";
            }
            case Map: {
                if (args.size() < 2) return "map<?, ?>";
                return "map<" + args[0]->toString() + ", " + args[1]->toString() + ">";
            }
            case Rlt: {
                if (args.size() < 2) return "rlt<?, ?>";
                return "rlt<" + args[0]->toString() + ", " + args[1]->toString() + ">";
            }
            case Tup: {
                std::string s = "tup<";
                for (size_t i = 0; i < args.size(); ++i) {
                    if (i) s += ", ";
                    s += args[i]->toString();
                }
                return s + ">";
            }
            case Fun: {
                std::string s = "(";
                for (size_t i = 0; i + 1 < args.size(); ++i) {
                    if (i) s += ", ";
                    s += args[i]->toString();
                }
                s += ") -> ";
                if (!args.empty()) s += args.back()->toString();
                return s;
            }
        }
        return "?";
    }
};

// ============================================================================
// P1-3  Token
// ============================================================================
enum class TT : uint16_t {
    Eof = 0,
    Newline, Indent, Dedent,

    Ident, IntLit, FltLit, ChrLit, StrLit,

    AsmRaw,
    Semicolon,

    Kw_fun, Kw_var, Kw_ent, Kw_ret, Kw_lam, Kw_cor, Kw_asm,
    Kw_if, Kw_els, Kw_whl, Kw_for, Kw_swt, Kw_cas, Kw_def,
    Kw_brk, Kw_cnt, Kw_jmp, Kw_mov, Kw_set, Kw_inc, Kw_dec,
    Kw_swp, Kw_new, Kw_del, Kw_put, Kw_prn, Kw_epu, Kw_epr,
    Kw_get, Kw_as, Kw_ok, Kw_er, Kw_try, Kw_unw, Kw_cal,

    Kw_int, Kw_flt, Kw_chr, Kw_bol, Kw_str, Kw_nil, Kw_cod,

    Kw_arr, Kw_map, Kw_opt, Kw_sig, Kw_rng, Kw_slc, Kw_tup,
    Kw_tck, Kw_rlt,

    Kw_rec, Kw_enm, Kw_uni, Kw_cls, Kw_mod, Kw_tra, Kw_typ,
    Kw_equ, Kw_pkg,

    Kw_pub, Kw_pri, Kw_pro, Kw_fnl, Kw_vir, Kw_abs, Kw_sta,
    Kw_ini, Kw_fin, Kw_frn, Kw_sup,

    Kw_reg, Kw_vol, Kw_con, Kw_ixl, Kw_aln, Kw_ntr, Kw_usr, Kw_whe,

    Kw_sext, Kw_cte, Kw_rep, Kw_alc, Kw_gen, Kw_lod, Kw_upd,
    Kw_exp, Kw_ext, Kw_use,

    Kw_add, Kw_sub, Kw_mul, Kw_div, Kw_pow, Kw_neg,
    Kw_min, Kw_max, Kw_shl, Kw_shr, Kw_lsr,
    Kw_and, Kw_or, Kw_xor, Kw_not,
    Kw_neq, Kw_grt, Kw_lss, Kw_geq, Kw_leq, Kw_sel,

    Kw_tru, Kw_fal,

    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Dot, Colon, At, Hash, Dollar, DollarAln,
    Amp, Pipe, Tilde, Bang,
    Plus, Minus, Star, Slash, Percent, Caret, Assign,
    Lt, Gt, EqEq, Neq, Leq, Geq, AndAnd, OrOr,
    DotDot, DotDotLt, Arrow, FatArrow, PipeGt, Shl, Shr, LsrOp,
};

struct Token {
    TT          type = TT::Eof;
    std::string text;
    int         line = 1;
    int         col  = 1;

    int64_t     ival = 0;
    double      fval = 0;
    char        cval = 0;
    std::string sval;
};

inline const std::unordered_map<std::string, TT>& keywordTable() {
    static const std::unordered_map<std::string, TT> t = {
        {"fun",TT::Kw_fun},{"var",TT::Kw_var},{"ent",TT::Kw_ent},{"ret",TT::Kw_ret},
        {"lam",TT::Kw_lam},{"cor",TT::Kw_cor},{"asm",TT::Kw_asm},{"if",TT::Kw_if},
        {"els",TT::Kw_els},{"whl",TT::Kw_whl},{"for",TT::Kw_for},{"swt",TT::Kw_swt},
        {"cas",TT::Kw_cas},{"def",TT::Kw_def},{"brk",TT::Kw_brk},{"cnt",TT::Kw_cnt},
        {"jmp",TT::Kw_jmp},{"mov",TT::Kw_mov},{"set",TT::Kw_set},{"inc",TT::Kw_inc},
        {"dec",TT::Kw_dec},{"swp",TT::Kw_swp},{"new",TT::Kw_new},{"del",TT::Kw_del},
        {"put",TT::Kw_put},{"prn",TT::Kw_prn},{"epu",TT::Kw_epu},{"epr",TT::Kw_epr},
        {"get",TT::Kw_get},{"as",TT::Kw_as},{"ok",TT::Kw_ok},{"er",TT::Kw_er},
        {"try",TT::Kw_try},{"unw",TT::Kw_unw},{"cal",TT::Kw_cal},

        {"int",TT::Kw_int},{"flt",TT::Kw_flt},{"chr",TT::Kw_chr},{"bol",TT::Kw_bol},
        {"str",TT::Kw_str},{"nil",TT::Kw_nil},{"cod",TT::Kw_cod},

        {"arr",TT::Kw_arr},{"map",TT::Kw_map},{"opt",TT::Kw_opt},{"sig",TT::Kw_sig},
        {"rng",TT::Kw_rng},{"slc",TT::Kw_slc},{"tup",TT::Kw_tup},{"tck",TT::Kw_tck},
        {"rlt",TT::Kw_rlt},

        {"rec",TT::Kw_rec},{"enm",TT::Kw_enm},{"uni",TT::Kw_uni},{"cls",TT::Kw_cls},
        {"mod",TT::Kw_mod},{"tra",TT::Kw_tra},{"typ",TT::Kw_typ},
        {"equ",TT::Kw_equ},{"pkg",TT::Kw_pkg},

        {"pub",TT::Kw_pub},{"pri",TT::Kw_pri},{"pro",TT::Kw_pro},{"fnl",TT::Kw_fnl},
        {"vir",TT::Kw_vir},{"abs",TT::Kw_abs},{"sta",TT::Kw_sta},{"ini",TT::Kw_ini},
        {"fin",TT::Kw_fin},{"frn",TT::Kw_frn},{"sup",TT::Kw_sup},

        {"reg",TT::Kw_reg},{"vol",TT::Kw_vol},{"con",TT::Kw_con},{"ixl",TT::Kw_ixl},
        {"aln",TT::Kw_aln},{"ntr",TT::Kw_ntr},{"usr",TT::Kw_usr},{"whe",TT::Kw_whe},

        {"sext",TT::Kw_sext},{"cte",TT::Kw_cte},{"rep",TT::Kw_rep},{"alc",TT::Kw_alc},
        {"gen",TT::Kw_gen},{"lod",TT::Kw_lod},{"upd",TT::Kw_upd},{"exp",TT::Kw_exp},
        {"ext",TT::Kw_ext},{"use",TT::Kw_use},

        {"add",TT::Kw_add},{"sub",TT::Kw_sub},{"mul",TT::Kw_mul},{"div",TT::Kw_div},
        {"pow",TT::Kw_pow},{"neg",TT::Kw_neg},{"min",TT::Kw_min},{"max",TT::Kw_max},
        {"shl",TT::Kw_shl},{"shr",TT::Kw_shr},{"lsr",TT::Kw_lsr},
        {"and",TT::Kw_and},{"or",TT::Kw_or},{"xor",TT::Kw_xor},{"not",TT::Kw_not},
        {"neq",TT::Kw_neq},{"grt",TT::Kw_grt},{"lss",TT::Kw_lss},
        {"geq",TT::Kw_geq},{"leq",TT::Kw_leq},{"sel",TT::Kw_sel},

        {"tru",TT::Kw_tru},{"fal",TT::Kw_fal},
    };
    return t;
}

inline bool tokenStartsType(TT t) {
    switch (t) {
        case TT::Kw_int: case TT::Kw_flt: case TT::Kw_chr:
        case TT::Kw_bol: case TT::Kw_str: case TT::Kw_nil:
        case TT::Kw_cod:
        case TT::Kw_arr: case TT::Kw_map: case TT::Kw_opt:
        case TT::Kw_sig: case TT::Kw_rng: case TT::Kw_slc:
        case TT::Kw_tup: case TT::Kw_tck: case TT::Kw_rlt:
        case TT::Star: case TT::Caret: case TT::LParen:
        case TT::Ident:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// P2  词法分析器 Lexer
// ============================================================================
class Lexer {
public:
    explicit Lexer(std::string src) : src_(std::move(src)) {}

    std::vector<Token> tokenize();

private:
    std::string src_;
    size_t      pos_  = 0;
    int         line_ = 1;
    int         col_  = 1;

    char peek(size_t off = 0) const {
        size_t p = pos_ + off;
        return p < src_.size() ? src_[p] : '\0';
    }

    char adv() {
        if (pos_ >= src_.size()) return '\0';
        char c = src_[pos_++];
        if (c == '\n') { ++line_; col_ = 1; }
        else           { ++col_; }
        return c;
    }

    bool eof() const { return pos_ >= src_.size(); }

    Token mk(TT t, int ln, int cl) {
        Token tok;
        tok.type = t;
        tok.line = ln;
        tok.col  = cl;
        return tok;
    }

    Token mkOp(TT t, int len, int ln, int cl) {
        Token tok;
        tok.type = t;
        tok.line = ln;
        tok.col  = cl;
        for (int i = 0; i < len; ++i) tok.text += peek(static_cast<size_t>(i));
        for (int i = 0; i < len; ++i) adv();
        return tok;
    }

    bool skipBlockComment();
    bool scanLineHasCode();
    Token captureAsmBlock(int startLine, int startCol);

    Token lexNumber();
    Token lexIdent();
    Token lexChar();
    Token lexString();
    void  readStringBody(std::string& buf, char quote, int sl, int sc);
    Token lexOperator();
    bool forHeaderMode_ = false;
};

inline bool Lexer::skipBlockComment() {
    adv(); adv();
    int depth = 1;
    while (!eof() && depth > 0) {
        if (peek() == '{' && peek(1) == '*') {
            adv(); adv(); ++depth;
        } else if (peek() == '*' && peek(1) == '}') {
            adv(); adv(); --depth;
        } else {
            adv();
        }
    }
    if (depth > 0)
        throw ArcError::at(line_, col_, "unterminated block comment");

    size_t p = pos_;
    while (p < src_.size() && (src_[p] == ' ' || src_[p] == '\t')) ++p;
    return (p >= src_.size() || src_[p] == '\n');
}

inline bool Lexer::scanLineHasCode() {
    size_t sp = pos_;
    int    sl = line_;
    int    sc = col_;

    while (true) {
        while (peek() == ' ' || peek() == '\t') adv();
        if (peek() == '{' && peek(1) == '*') {
            adv(); adv();
            int depth = 1;
            while (!eof() && depth > 0) {
                if (peek() == '{' && peek(1) == '*') { adv(); adv(); ++depth; }
                else if (peek() == '*' && peek(1) == '}') { adv(); adv(); --depth; }
                else adv();
            }
            if (depth > 0) {
                pos_ = sp; line_ = sl; col_ = sc;
                return false;
            }
            continue;
        }
        break;
    }

    char c = peek();
    bool has = (c != '\n' && c != '\0' && c != ';');

    pos_ = sp; line_ = sl; col_ = sc;
    return has;
}

inline Token Lexer::captureAsmBlock(int startLine, int startCol) {
    Token t;
    t.type = TT::AsmRaw;
    t.line = startLine;
    t.col  = startCol;

    int depth = 0;
    while (!eof()) {
        char c = adv();
        t.text += c;
        if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0) return t;
        }
    }
    throw ArcError::at(startLine, startCol, "unterminated asm block");
}

inline std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    std::vector<int>   indents = {0};
    bool atLineStart = true;

    while (!eof()) {
        if (atLineStart) {
            int indent = 0;
            while (peek() == ' ') { adv(); ++indent; }
            if (peek() == '\t')
                throw ArcError::at(line_, col_,
                    "tab character is not allowed for indentation");

            bool hasCode = scanLineHasCode();

            if (!hasCode) {
                while (!eof() && peek() != '\n') {
                    if (peek() == '{' && peek(1) == '*') {
                        skipBlockComment();
                    } else {
                        adv();
                    }
                }
                if (!eof()) adv();
                continue;
            }

            if (indent > indents.back()) {
                indents.push_back(indent);
                out.push_back(mk(TT::Indent, line_, col_));
            } else if (indent < indents.back()) {
                while (indent < indents.back()) {
                    indents.pop_back();
                    out.push_back(mk(TT::Dedent, line_, col_));
                }
                if (indent != indents.back()) {
                    throw ArcError::at(line_, col_,
                        "indentation mismatch: expected "
                        + std::to_string(indents.back())
                        + " spaces, got " + std::to_string(indent));
                }
            }
            atLineStart = false;
        }

        while (peek() == ' ' || peek() == '\t') adv();
        if (eof()) break;

        char c = peek();

        if (c == '\n') {
            out.push_back(mk(TT::Newline, line_, col_));
            adv();
            atLineStart = true;
            forHeaderMode_ = false;
            continue;
        }

        if (c == ';') {
            if (forHeaderMode_) {
                out.push_back(mkOp(TT::Semicolon, 1, line_, col_));
                continue;
            }
            while (!eof() && peek() != '\n') adv();
            continue;
        }

        if (c == '{' && peek(1) == '*') {
            bool endAtLineStart = skipBlockComment();
            if (endAtLineStart) atLineStart = true;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            out.push_back(lexNumber());
            continue;
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            int tokLine = line_, tokCol = col_;
            Token t = lexIdent();

            if (t.type == TT::Kw_asm) {
                size_t probePos  = pos_;
                int    probeLine = line_;
                int    probeCol  = col_;

                while (peek() == ' ' || peek() == '\t') adv();

                if (peek() == '{') {
                    Token raw = captureAsmBlock(tokLine, tokCol);
                    out.push_back(t);
                    out.push_back(raw);
                    continue;
                }

                pos_  = probePos;
                line_ = probeLine;
                col_  = probeCol;
            }

            out.push_back(t);
            continue;
        }

        if (c == '\'') { out.push_back(lexChar());   continue; }
        if (c == '"')  { out.push_back(lexString()); continue; }

        {
            Token t = lexOperator();
            if (t.type == TT::Colon) forHeaderMode_ = false;
            if (t.type == TT::LBrace) forHeaderMode_ = false;
            out.push_back(std::move(t));
        }
    }

    int eofLine = line_;
    int eofCol  = col_;
    if (!out.empty()) {
        eofLine = out.back().line;
        eofCol  = out.back().col;
    }

    if (!out.empty() && out.back().type != TT::Newline)
        out.push_back(mk(TT::Newline, eofLine, eofCol));
    while (indents.size() > 1) {
        indents.pop_back();
        out.push_back(mk(TT::Dedent, eofLine, eofCol));
    }
    out.push_back(mk(TT::Eof, eofLine, eofCol));

    return out;
}

inline Token Lexer::lexNumber() {
    int sl = line_, sc = col_;
    std::string buf;
    bool isFloat = false;
    bool isHex   = false;
    bool isBin   = false;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        isHex = true;
        adv(); adv();
        while (std::isxdigit(static_cast<unsigned char>(peek())) || peek() == '_') {
            if (peek() != '_') buf += peek();
            adv();
        }
        if (buf.empty())
            throw ArcError::at(sl, sc, "empty hex literal");
    } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        isBin = true;
        adv(); adv();
        while (peek() == '0' || peek() == '1' || peek() == '_') {
            if (peek() != '_') buf += peek();
            adv();
        }
        if (buf.empty())
            throw ArcError::at(sl, sc, "empty binary literal");
    } else {
        while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_') {
            if (peek() != '_') buf += peek();
            adv();
        }
        if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            isFloat = true;
            buf += '.';
            adv();
            while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_') {
                if (peek() != '_') buf += peek();
                adv();
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            isFloat = true;
            int expLine = line_, expCol = col_;
            buf += 'e';
            adv();
            if (peek() == '+' || peek() == '-') {
                buf += peek();
                adv();
            }
            if (!std::isdigit(static_cast<unsigned char>(peek())))
                throw ArcError::at(expLine, expCol, "malformed exponent");
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                buf += peek();
                adv();
            }
        }
    }

    Token t;
    t.line = sl;
    t.col  = sc;
    t.text = buf;

    try {
        if (isFloat) {
            t.type = TT::FltLit;
            t.fval = std::stod(buf);
        } else if (isHex) {
            t.type = TT::IntLit;
            t.ival = static_cast<int64_t>(std::stoull(buf, nullptr, 16));
        } else if (isBin) {
            t.type = TT::IntLit;
            t.ival = static_cast<int64_t>(std::stoull(buf, nullptr, 2));
        } else {
            t.type = TT::IntLit;
            t.ival = static_cast<int64_t>(std::stoll(buf, nullptr, 10));
        }
    } catch (const std::exception&) {
        throw ArcError::at(sl, sc, "numeric literal out of range: " + buf);
    }

    return t;
}

inline Token Lexer::lexIdent() {
    int sl = line_, sc = col_;
    std::string buf;

    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
        buf += peek();
        adv();
    }

    Token t;
    t.line = sl;
    t.col  = sc;
    t.text = buf;

    const auto& kw = keywordTable();
    auto it = kw.find(buf);
    t.type = (it != kw.end()) ? it->second : TT::Ident;

    if (t.type == TT::Kw_for) forHeaderMode_ = true;

    return t;
}

inline void Lexer::readStringBody(std::string& buf, char quote, int sl, int sc) {
    while (!eof() && peek() != quote) {
        char c = adv();

        if (c == '\n') {
            if (quote == '\'')
                throw ArcError::at(sl, sc, "unterminated character literal");
            throw ArcError::at(sl, sc,
                "newline in string literal (use \\n instead)");
        }

        if (c != '\\') { buf += c; continue; }

        if (eof())
            throw ArcError::at(sl, sc, "unterminated escape sequence");

        char e = adv();
        switch (e) {
            case 'n':  buf += '\n'; break;
            case 't':  buf += '\t'; break;
            case 'r':  buf += '\r'; break;
            case '0':  buf += '\0'; break;
            case '\\': buf += '\\'; break;
            case '\'': buf += '\''; break;
            case '"':  buf += '"';  break;
            case 'x': {
                int v = 0;
                for (int i = 0; i < 2; ++i) {
                    int hLine = line_, hCol = col_;
                    char h = adv();
                    if (std::isdigit(static_cast<unsigned char>(h))) {
                        v = v * 16 + (h - '0');
                    } else if (std::isxdigit(static_cast<unsigned char>(h))) {
                        v = v * 16 +
                            (std::tolower(static_cast<unsigned char>(h)) - 'a' + 10);
                    } else {
                        throw ArcError::at(hLine, hCol,
                            "invalid hex escape sequence");
                    }
                }
                buf += static_cast<char>(v);
                break;
            }
            default:
                buf += e;
                break;
        }
    }
    if (eof())
        throw ArcError::at(sl, sc, "unterminated string literal");
    adv();
}

inline Token Lexer::lexChar() {
    int sl = line_, sc = col_;
    adv();
    std::string buf;
    readStringBody(buf, '\'', sl, sc);

    if (buf.size() != 1)
        throw ArcError::at(sl, sc,
            "character literal must contain exactly one character");

    Token t;
    t.type = TT::ChrLit;
    t.line = sl;
    t.col  = sc;
    t.cval = buf[0];
    t.text = buf;
    return t;
}

inline Token Lexer::lexString() {
    int sl = line_, sc = col_;
    adv();

    std::string buf;
    readStringBody(buf, '"', sl, sc);

    while (true) {
        size_t sp  = pos_;
        int    sl2 = line_;
        int    sc2 = col_;

        while (true) {
            while (peek() == ' ' || peek() == '\t') adv();
            if (peek() == '{' && peek(1) == '*') {
                skipBlockComment();
                continue;
            }
            break;
        }

        if (peek() == '"') {
            adv();
            readStringBody(buf, '"', sl2, sc2);
            continue;
        }

        pos_  = sp;
        line_ = sl2;
        col_  = sc2;
        break;
    }

    Token t;
    t.type = TT::StrLit;
    t.line = sl;
    t.col  = sc;
    t.sval = buf;
    t.text = buf;
    return t;
}

inline Token Lexer::lexOperator() {
    int sl = line_, sc = col_;
    char c0 = peek(0), c1 = peek(1), c2 = peek(2), c3 = peek(3), c4 = peek(4);

    if (c0 == '$' && c1 == 'a' && c2 == 'l' && c3 == 'n') {
        bool boundary = !(std::isalnum(static_cast<unsigned char>(c4)) || c4 == '_');
        if (boundary)
            return mkOp(TT::DollarAln, 4, sl, sc);
    }

    if (c0 == '>' && c1 == '>' && c2 == '>') return mkOp(TT::LsrOp,    3, sl, sc);
    if (c0 == '.' && c1 == '.' && c2 == '<') return mkOp(TT::DotDotLt, 3, sl, sc);

    if (c0 == '=' && c1 == '=') return mkOp(TT::EqEq,     2, sl, sc);
    if (c0 == '!' && c1 == '=') return mkOp(TT::Neq,      2, sl, sc);
    if (c0 == '<' && c1 == '=') return mkOp(TT::Leq,      2, sl, sc);
    if (c0 == '>' && c1 == '=') return mkOp(TT::Geq,      2, sl, sc);
    if (c0 == '&' && c1 == '&') return mkOp(TT::AndAnd,   2, sl, sc);
    if (c0 == '|' && c1 == '|') return mkOp(TT::OrOr,     2, sl, sc);
    if (c0 == '.' && c1 == '.') return mkOp(TT::DotDot,   2, sl, sc);
    if (c0 == '-' && c1 == '>') return mkOp(TT::Arrow,    2, sl, sc);
    if (c0 == '=' && c1 == '>') return mkOp(TT::FatArrow, 2, sl, sc);
    if (c0 == '|' && c1 == '>') return mkOp(TT::PipeGt,   2, sl, sc);
    if (c0 == '<' && c1 == '<') return mkOp(TT::Shl,      2, sl, sc);
    if (c0 == '>' && c1 == '>') return mkOp(TT::Shr,      2, sl, sc);

    TT type;
    switch (c0) {
        case '(': type = TT::LParen;   break;
        case ')': type = TT::RParen;   break;
        case '{': type = TT::LBrace;   break;
        case '}': type = TT::RBrace;   break;
        case '[': type = TT::LBracket; break;
        case ']': type = TT::RBracket; break;
        case ',': type = TT::Comma;    break;
        case '.': type = TT::Dot;      break;
        case ':': type = TT::Colon;    break;
        case '@': type = TT::At;       break;
        case '#': type = TT::Hash;     break;
        case '$': type = TT::Dollar;   break;
        case '&': type = TT::Amp;      break;
        case '|': type = TT::Pipe;     break;
        case '~': type = TT::Tilde;    break;
        case '!': type = TT::Bang;     break;
        case '+': type = TT::Plus;     break;
        case '-': type = TT::Minus;    break;
        case '*': type = TT::Star;     break;
        case '/': type = TT::Slash;    break;
        case '%': type = TT::Percent;  break;
        case '^': type = TT::Caret;    break;
        case '=': type = TT::Assign;   break;
        case '<': type = TT::Lt;       break;
        case '>': type = TT::Gt;       break;
        default:
            throw ArcError::at(sl, sc,
                std::string("unexpected character '") + c0 + "'");
    }
    return mkOp(type, 1, sl, sc);
}

// P2 结束。namespace arc 保持打开。
// ============================================================================
// P3  预处理器 Preprocessor
// ============================================================================
class Preprocessor {
public:
    explicit Preprocessor(bool verbose = false) : verbose_(verbose) {}

    std::string processFile(const std::string& path) {
        std::string origin = normalizePath(path);
        std::string src    = loadFile(path);
        return expand(src, origin);
    }

    std::string processSource(const std::string& src, const std::string& origin) {
        return expand(src, normalizePath(origin));
    }

    bool hadWarning() const { return hadWarning_; }

private:
    using MacroVal = std::variant<int64_t, std::string>;

    struct CondFrame {
        bool parentActive = true;
        bool active       = true;
        bool taken        = false;
        bool inElse       = false;
        int  startLine    = 1;
    };

    std::unordered_map<std::string, MacroVal> macros_;
    std::vector<CondFrame>                    condStack_;
    std::unordered_set<std::string>           onceSet_;
    std::unordered_set<std::string>           inProgress_;
    bool hadWarning_ = false;
    bool verbose_    = false;

    std::string currentFile_;

    [[noreturn]] void err(int ln, const std::string& msg) const {
        std::string full;
        if (!currentFile_.empty())
            full += currentFile_ + ":";
        full += std::to_string(ln) + ": " + msg;
        throw ArcError(std::move(full), ln, 0);
    }

    std::string expand(const std::string& src, const std::string& origin) {
        if (onceSet_.count(origin)) return "";
        if (inProgress_.count(origin))
            throw ArcError("circular include detected: " + origin);
        inProgress_.insert(origin);

        std::string savedFile = currentFile_;
        currentFile_ = origin;

        size_t condBase = condStack_.size();

        std::ostringstream out;
        size_t p = 0;
        int    lineNo = 1;

        while (p < src.size()) {
            size_t e = src.find('\n', p);
            bool hasNewline = (e != std::string::npos);

            std::string line;
            if (hasNewline) {
                line = src.substr(p, e - p);
                p = e + 1;
            } else {
                line = src.substr(p);
                p = src.size();
            }

            size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;

            if (i < line.size() && line[i] == '#') {
                processDirective(line.substr(i + 1), origin, out, lineNo);
            } else {
                if (condActive()) out << line;
                out << "\n";
            }

            ++lineNo;
            if (!hasNewline) break;
        }

        if (condStack_.size() != condBase) {
            int ln = condStack_.empty() ? lineNo : condStack_.back().startLine;
            err(ln, "unterminated #if");
        }

        inProgress_.erase(origin);
        currentFile_ = savedFile;
        return out.str();
    }

    bool condActive() const {
        for (const auto& f : condStack_) if (!f.active) return false;
        return true;
    }

    void processDirective(const std::string& body,
                          const std::string& origin,
                          std::ostringstream& out,
                          int lineNo) {
        size_t i = 0;
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
        size_t j = i;
        while (j < body.size() &&
               (std::isalpha(static_cast<unsigned char>(body[j])) || body[j] == '_')) {
            ++j;
        }
        std::string kw   = body.substr(i, j - i);
        std::string rest = body.substr(j);

        bool active = condActive();

        if (kw == "if")     { handleIf(rest, active, lineNo);     out << "\n"; return; }
        if (kw == "ifdef")  { handleIfdef(rest, active, false, lineNo); out << "\n"; return; }
        if (kw == "ifndef") { handleIfdef(rest, active, true,  lineNo); out << "\n"; return; }
        if (kw == "els")    { handleEls(lineNo);                  out << "\n"; return; }
        if (kw == "end")    { handleEnd(lineNo);                  out << "\n"; return; }

        if (!active) { out << "\n"; return; }

        if (kw == "inc")    { handleInc(rest, origin, out, lineNo); return; }
        if (kw == "def")    { handleDef(rest, lineNo);            out << "\n"; return; }
        if (kw == "onc")    { handleOnc(origin);                  out << "\n"; return; }
        if (kw == "err")    { handleErr(rest, lineNo);            }
        if (kw == "wrn")    { handleWrn(rest, lineNo);            out << "\n"; return; }
        if (kw == "assert") { handleAssert(rest, lineNo);         out << "\n"; return; }
        if (kw.empty())     {                                     out << "\n"; return; }

        err(lineNo, "unknown preprocessor directive: #" + kw);
    }

    void handleIf(const std::string& rest, bool parentActive, int lineNo) {
        bool cond = parentActive ? (evalExpr(rest, lineNo) != 0) : false;
        CondFrame f;
        f.parentActive = parentActive;
        f.active       = parentActive && cond;
        f.taken        = f.active;
        f.inElse       = false;
        f.startLine    = lineNo;
        condStack_.push_back(f);
    }

    void handleIfdef(const std::string& rest, bool parentActive, bool negate, int lineNo) {
        std::string name = trim(rest);
        if (name.empty()) err(lineNo, "#ifdef/#ifndef requires an identifier");
        bool defined = (macros_.count(name) > 0);
        bool cond    = negate ? !defined : defined;
        CondFrame f;
        f.parentActive = parentActive;
        f.active       = parentActive && cond;
        f.taken        = f.active;
        f.inElse       = false;
        f.startLine    = lineNo;
        condStack_.push_back(f);
    }

    void handleEls(int lineNo) {
        if (condStack_.empty()) err(lineNo, "#els without matching #if");
        CondFrame& f = condStack_.back();
        if (f.inElse)
            err(lineNo, "duplicate #els (matching #if at line "
                        + std::to_string(f.startLine) + ")");
        f.inElse = true;

        if (!f.parentActive) {
            f.active = false;
        } else if (f.taken) {
            f.active = false;
        } else {
            f.active = true;
            f.taken  = true;
        }
    }

    void handleEnd(int lineNo) {
        if (condStack_.empty()) err(lineNo, "#end without matching #if");
        condStack_.pop_back();
    }

    void handleInc(const std::string& rest,
                   const std::string& origin,
                   std::ostringstream& out,
                   int lineNo) {
        std::string trimmed = trim(rest);
        std::string target = parseIncTarget(rest);
        if (target.empty()) err(lineNo, "#inc requires a path");

        bool systemForm = (!trimmed.empty() && trimmed[0] == '<');

        std::string full;
        if (systemForm) {
            static const char* sysDirs[] = {
                "/usr/local/include",
                "/usr/include",
            };
            for (auto d : sysDirs) {
                std::string cand = std::string(d) + "/" + target;
                std::ifstream f(cand);
                if (f) { full = cand; break; }
            }
            if (full.empty())
                err(lineNo, "#inc: cannot find system header: " + target);
        } else {
            std::string dir = dirOf(origin);
            full = joinPath(dir, target);
        }

        std::string content = loadFile(full, lineNo);
        std::string abs     = normalizePath(full);
        out << expand(content, abs);
    }

    void handleDef(const std::string& rest, int lineNo) {
        std::string s = trim(rest);
        size_t i = 0;
        while (i < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) {
            ++i;
        }
        std::string name = s.substr(0, i);
        if (name.empty()) err(lineNo, "#def requires an identifier");

        std::string expr = trim(s.substr(i));
        if (expr.empty()) {
            macros_[name] = int64_t(1);
            return;
        }

        if (expr[0] == '"') {
            size_t e = expr.find('"', 1);
            if (e == std::string::npos)
                err(lineNo, "#def: unterminated string");
            macros_[name] = expr.substr(1, e - 1);
            return;
        }

        bool isIdent = !expr.empty() &&
                       (std::isalpha(static_cast<unsigned char>(expr[0])) ||
                        expr[0] == '_');
        if (isIdent) {
            for (size_t k = 1; k < expr.size(); ++k) {
                if (!std::isalnum(static_cast<unsigned char>(expr[k])) &&
                    expr[k] != '_') { isIdent = false; break; }
            }
        }
        if (isIdent && !macros_.count(expr)) {
            macros_[name] = expr;
            return;
        }

        macros_[name] = evalExpr(expr, lineNo);
    }

    void handleOnc(const std::string& origin) {
        onceSet_.insert(origin);
    }

    void handleErr(const std::string& rest, int lineNo) {
        err(lineNo, "preprocessor error: " + parseMessage(rest, lineNo));
    }

    void handleWrn(const std::string& rest, int lineNo) {
        std::cerr << currentFile_ << ":" << lineNo
                  << ": arc warning: " << parseMessage(rest, lineNo) << "\n";
        hadWarning_ = true;
    }

    void handleAssert(const std::string& rest, int lineNo) {
        std::string s = trim(rest);
        size_t comma = findTopLevelComma(s);

        std::string exprStr = (comma == std::string::npos)
                              ? s : trim(s.substr(0, comma));
        std::string msg     = (comma == std::string::npos)
                              ? std::string("assertion failed")
                              : parseMessage(s.substr(comma + 1), lineNo);

        if (evalExpr(exprStr, lineNo) == 0)
            err(lineNo, "preprocessor assert failed: " + msg);
    }

    int64_t evalExpr(const std::string& s, int lineNo) const {
        PPEvaluator ev{s, 0, macros_, currentFile_, lineNo};
        auto v = ev.parseOr();
        ev.skip();
        if (!ev.eof())
            ev.fail("preprocessor expression: trailing characters");
        return ppInt(v);
    }

    struct PPEvaluator {
        const std::string& s;
        size_t p = 0;
        const std::unordered_map<std::string, MacroVal>& macros;
        const std::string& file;
        int lineNo;

        void skip() {
            while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
        }
        bool eof() { skip(); return p >= s.size(); }

        [[noreturn]] void fail(const std::string& msg) const {
            throw ArcError(file + ":" + std::to_string(lineNo) + ": " + msg);
        }

        bool tryMatch(char c) {
            skip();
            if (p < s.size() && s[p] == c) { ++p; return true; }
            return false;
        }
        bool tryMatch2(char a, char b) {
            skip();
            if (p + 1 < s.size() && s[p] == a && s[p + 1] == b) { p += 2; return true; }
            return false;
        }

        MacroVal parseOr() {
            MacroVal l = parseAnd();
            while (tryMatch2('|', '|')) {
                MacroVal r = parseAnd();
                l = int64_t(ppBool(l) || ppBool(r) ? 1 : 0);
            }
            return l;
        }
        MacroVal parseAnd() {
            MacroVal l = parseEq();
            while (tryMatch2('&', '&')) {
                MacroVal r = parseEq();
                l = int64_t(ppBool(l) && ppBool(r) ? 1 : 0);
            }
            return l;
        }
        MacroVal parseEq() {
            MacroVal l = parseRelational();
            while (true) {
                if (tryMatch2('=', '=')) {
                    MacroVal r = parseRelational();
                    l = int64_t(ppEq(l, r) ? 1 : 0);
                } else if (tryMatch2('!', '=')) {
                    MacroVal r = parseRelational();
                    l = int64_t(!ppEq(l, r) ? 1 : 0);
                } else {
                    break;
                }
            }
            return l;
        }

        MacroVal parseRelational() {
            MacroVal l = parseAdditive();
            while (true) {
                if (tryMatch2('<', '=')) {
                    MacroVal r = parseAdditive();
                    l = int64_t(ppInt(l) <= ppInt(r) ? 1 : 0);
                } else if (tryMatch2('>', '=')) {
                    MacroVal r = parseAdditive();
                    l = int64_t(ppInt(l) >= ppInt(r) ? 1 : 0);
                } else if (tryMatch('<')) {
                    MacroVal r = parseAdditive();
                    l = int64_t(ppInt(l) < ppInt(r) ? 1 : 0);
                } else if (tryMatch('>')) {
                    MacroVal r = parseAdditive();
                    l = int64_t(ppInt(l) > ppInt(r) ? 1 : 0);
                } else {
                    break;
                }
            }
            return l;
        }

        MacroVal parseAdditive() {
            MacroVal l = parseUnary();
            while (true) {
                if (tryMatch('+')) {
                    MacroVal r = parseUnary();
                    l = int64_t(ppInt(l) + ppInt(r));
                } else if (tryMatch('-')) {
                    MacroVal r = parseUnary();
                    l = int64_t(ppInt(l) - ppInt(r));
                } else {
                    break;
                }
            }
            return l;
        }
        MacroVal parseUnary() {
            if (tryMatch('!')) {
                MacroVal v = parseUnary();
                return int64_t(ppBool(v) ? 0 : 1);
            }
            if (tryMatch('-')) {
                MacroVal v = parseUnary();
                return int64_t(-ppInt(v));
            }
            if (tryMatch('+')) return parseUnary();
            return parsePrimary();
        }
        MacroVal parsePrimary() {
            skip();
            if (p >= s.size())
                fail("preprocessor expression: unexpected end");
            char c = s[p];

            if (c == '(') {
                ++p;
                MacroVal v = parseOr();
                skip();
                if (p >= s.size() || s[p] != ')')
                    fail("preprocessor expression: missing ')'");
                ++p;
                return v;
            }

            if (std::isdigit(static_cast<unsigned char>(c))) {
                size_t e = p;
                while (e < s.size() &&
                       (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_')) {
                    ++e;
                }
                std::string lit = s.substr(p, e - p);
                p = e;
                std::string clean;
                for (char ch : lit) if (ch != '_') clean += ch;
                try {
                    if (clean.size() > 2 && clean[0] == '0' &&
                        (clean[1] == 'x' || clean[1] == 'X')) {
                        return int64_t(std::stoull(clean.substr(2), nullptr, 16));
                    }
                    if (clean.size() > 2 && clean[0] == '0' &&
                        (clean[1] == 'b' || clean[1] == 'B')) {
                        return int64_t(std::stoull(clean.substr(2), nullptr, 2));
                    }
                    return int64_t(std::stoll(clean, nullptr, 10));
                } catch (const std::exception&) {
                    fail("preprocessor expression: bad number: " + lit);
                }
            }

            if (c == '"') {
                ++p;
                std::string v;
                while (p < s.size() && s[p] != '"') {
                    if (s[p] == '\\' && p + 1 < s.size()) {
                        char n = s[p + 1];
                        if      (n == 'n')  v += '\n';
                        else if (n == 't')  v += '\t';
                        else if (n == 'r')  v += '\r';
                        else if (n == '\\') v += '\\';
                        else if (n == '"')  v += '"';
                        else                v += n;
                        p += 2;
                    } else {
                        v += s[p++];
                    }
                }
                if (p >= s.size())
                    fail("preprocessor expression: unterminated string");
                ++p;
                return v;
            }

            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t e = p;
                while (e < s.size() &&
                       (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_')) {
                    ++e;
                }
                std::string name = s.substr(p, e - p);
                p = e;

                if (name == "defined") {
                    skip();
                    if (p >= s.size() || s[p] != '(')
                        fail("defined requires (...)");
                    ++p;
                    skip();
                    size_t e2 = p;
                    while (e2 < s.size() &&
                           (std::isalnum(static_cast<unsigned char>(s[e2])) || s[e2] == '_')) {
                        ++e2;
                    }
                    std::string inner = s.substr(p, e2 - p);
                    p = e2;
                    skip();
                    if (p >= s.size() || s[p] != ')')
                        fail("defined requires (...)");
                    ++p;
                    return int64_t(macros.count(inner) > 0 ? 1 : 0);
                }

                auto it = macros.find(name);
                if (it == macros.end()) return int64_t(0);

                MacroVal val = it->second;
                std::unordered_set<std::string> visited;
                visited.insert(name);
                while (std::holds_alternative<std::string>(val)) {
                    const std::string& v = std::get<std::string>(val);
                    if (v.empty()) break;
                    if (!(std::isalpha(static_cast<unsigned char>(v[0])) || v[0] == '_'))
                        break;
                    if (visited.count(v)) break;
                    visited.insert(v);
                    auto it2 = macros.find(v);
                    if (it2 == macros.end()) break;
                    val = it2->second;
                }
                return val;
            }

            fail(std::string("preprocessor expression: unexpected character '")
                 + c + "'");
        }
    };

    static int64_t ppInt(const MacroVal& v) {
        if (std::holds_alternative<int64_t>(v)) return std::get<int64_t>(v);
        return std::get<std::string>(v).empty() ? 0 : 1;
    }
    static bool ppBool(const MacroVal& v) { return ppInt(v) != 0; }
    static bool ppEq(const MacroVal& a, const MacroVal& b) {
        if (std::holds_alternative<std::string>(a) &&
            std::holds_alternative<std::string>(b)) {
            return std::get<std::string>(a) == std::get<std::string>(b);
        }
        return ppInt(a) == ppInt(b);
    }

    static std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    }

    static size_t findTopLevelComma(const std::string& s) {
        int depth  = 0;
        bool inStr = false;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (inStr) {
                if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == '(') ++depth;
            else if (c == ')') --depth;
            else if (c == ',' && depth == 0) return i;
        }
        return std::string::npos;
    }

    std::string parseMessage(const std::string& rest, int lineNo) const {
        std::string s = trim(rest);
        if (!s.empty() && s[0] == '"') {
            size_t e = s.find('"', 1);
            if (e == std::string::npos)
                err(lineNo, "unterminated string in directive");
            return s.substr(1, e - 1);
        }
        return s;
    }

    std::string parseIncTarget(const std::string& rest) const {
        std::string s = trim(rest);
        if (s.empty()) return "";
        if (s[0] == '"') {
            size_t e = s.find('"', 1);
            if (e == std::string::npos)
                throw ArcError("#inc: unterminated quoted path");
            return s.substr(1, e - 1);
        }
        if (s[0] == '<') {
            size_t e = s.find('>', 1);
            if (e == std::string::npos)
                throw ArcError("#inc: unterminated <> path");
            return s.substr(1, e - 1);
        }
        return s;
    }

    std::string loadFile(const std::string& path, int lineNo = 0) const {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::string msg = "cannot open include file: " + path;
            if (lineNo > 0) err(lineNo, msg);
            throw ArcError(msg);
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    static std::string normalizePath(const std::string& p) {
        if (p.empty()) return ".";
        bool leadingSlash = (p[0] == '/');
        std::vector<std::string> parts;
        size_t i = 0;
        while (i <= p.size()) {
            size_t e = p.find('/', i);
            if (e == std::string::npos) e = p.size();
            std::string seg = p.substr(i, e - i);
            if (!seg.empty() && seg != ".") {
                if (seg == "..") {
                    if (!parts.empty() && parts.back() != "..") parts.pop_back();
                    else if (!leadingSlash) parts.push_back("..");
                } else {
                    parts.push_back(seg);
                }
            }
            i = e + 1;
            if (e == p.size()) break;
        }
        std::string out = leadingSlash ? "/" : "";
        for (size_t k = 0; k < parts.size(); ++k) {
            if (k) out += "/";
            out += parts[k];
        }
        return out.empty() ? "." : out;
    }

    static std::string dirOf(const std::string& absPath) {
        size_t pos = absPath.find_last_of('/');
        if (pos == std::string::npos) return ".";
        if (pos == 0) return "/";
        return absPath.substr(0, pos);
    }

    static std::string joinPath(const std::string& dir, const std::string& rel) {
        if (rel.empty()) return dir;
        if (rel[0] == '/') return rel;
        if (dir.empty()) return dir;
        if (dir == ".") return rel;
        return dir + "/" + rel;
    }
};

// P3 结束。namespace arc 保持打开。
// ============================================================================
// P4  抽象语法树 AST
// ============================================================================
struct Expr;
struct Stmt;
struct Function;

using ExprPtr     = std::unique_ptr<Expr>;
using StmtPtr     = std::unique_ptr<Stmt>;
using FunctionPtr = std::shared_ptr<Function>;

// ----------------------------------------------------------------------------
// P4-1  表达式
// ----------------------------------------------------------------------------
struct Expr {
    enum K {
        IntLit, FltLit, ChrLit, StrLit, BolLit, NilLit,
        Ident,
        Binary, Unary, NamedInstr,
        Call, MethodCall, CalCall,
        Index, Slice, Member, TupleIndex,
        OptHas, OptVal,
        AsConv,
        OkExpr, ErExpr, TryExpr, UnwExpr, OrExpr,
        ArrayLit, TupleLit, RecLit, ClsLit, EnmLit, RangeLit,
        SizeofExpr, AddrExpr, DerefExpr,
        SelExpr, BlkExpr, LamExpr, FmtExpr,
        PipeExpr, RseExpr, CteCall,
    } kind;
    int     line = 1;
    int     col  = 1;
    TypePtr ty;

    explicit Expr(K k) : kind(k) {}
    virtual ~Expr() = default;
};

struct IntExpr : Expr {
    int64_t v = 0;
    explicit IntExpr(int64_t x) : Expr(IntLit), v(x) {}
};
struct FltExpr : Expr {
    double v = 0;
    explicit FltExpr(double x) : Expr(FltLit), v(x) {}
};
struct ChrExpr : Expr {
    char v = 0;
    explicit ChrExpr(char x) : Expr(ChrLit), v(x) {}
};
struct StrExpr : Expr {
    std::string v;
    int rodataId = -1;
    explicit StrExpr(std::string x) : Expr(StrLit), v(std::move(x)) {}
};
struct BolExpr : Expr {
    bool v = false;
    explicit BolExpr(bool x) : Expr(BolLit), v(x) {}
};
struct NilExpr : Expr {
    NilExpr() : Expr(NilLit) {}
};

struct IdentExpr : Expr {
    std::string name;
    int  localSlot = -1;
    bool isGlobal  = false;
    explicit IdentExpr(std::string n) : Expr(Ident), name(std::move(n)) {}
};

struct BinaryExpr : Expr {
    std::string op;
    ExprPtr lhs, rhs;
    BinaryExpr(std::string o, ExprPtr l, ExprPtr r)
        : Expr(Binary), op(std::move(o)),
          lhs(std::move(l)), rhs(std::move(r)) {}
};

struct UnaryExpr : Expr {
    std::string op;
    ExprPtr operand;
    UnaryExpr(std::string o, ExprPtr e)
        : Expr(Unary), op(std::move(o)), operand(std::move(e)) {}
};

struct NamedInstrExpr : Expr {
    std::string name;
    std::vector<ExprPtr> args;
    explicit NamedInstrExpr(std::string n)
        : Expr(NamedInstr), name(std::move(n)) {}
};

struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    CallExpr(ExprPtr c, std::vector<ExprPtr> a)
        : Expr(Call), callee(std::move(c)), args(std::move(a)) {}
};

struct MethodCallExpr : Expr {
    ExprPtr receiver;
    std::string method;
    std::vector<ExprPtr> args;
    MethodCallExpr(ExprPtr r, std::string m, std::vector<ExprPtr> a)
        : Expr(MethodCall), receiver(std::move(r)),
          method(std::move(m)), args(std::move(a)) {}
};

struct CalCallExpr : Expr {
    ExprPtr target;
    std::vector<ExprPtr> args;
    CalCallExpr(ExprPtr t, std::vector<ExprPtr> a)
        : Expr(CalCall), target(std::move(t)), args(std::move(a)) {}
};

struct IndexExpr : Expr {
    ExprPtr container, index;
    IndexExpr(ExprPtr c, ExprPtr i)
        : Expr(Index), container(std::move(c)), index(std::move(i)) {}
};

struct SliceExpr : Expr {
    ExprPtr container;
    ExprPtr from;
    ExprPtr to;
    bool    exclusive = false;
    SliceExpr(ExprPtr c, ExprPtr f, ExprPtr t, bool ex)
        : Expr(Slice), container(std::move(c)),
          from(std::move(f)), to(std::move(t)), exclusive(ex) {}
};

struct MemberExpr : Expr {
    ExprPtr receiver;
    std::string name;
    MemberExpr(ExprPtr r, std::string n)
        : Expr(Member), receiver(std::move(r)), name(std::move(n)) {}
};

struct TupleIndexExpr : Expr {
    ExprPtr receiver;
    int     index = 0;
    TupleIndexExpr(ExprPtr r, int i)
        : Expr(TupleIndex), receiver(std::move(r)), index(i) {}
};

struct OptHasExpr : Expr {
    ExprPtr receiver;
    explicit OptHasExpr(ExprPtr r) : Expr(OptHas), receiver(std::move(r)) {}
};
struct OptValExpr : Expr {
    ExprPtr receiver;
    explicit OptValExpr(ExprPtr r) : Expr(OptVal), receiver(std::move(r)) {}
};

struct AsConvExpr : Expr {
    ExprPtr value;
    TypePtr target;
    AsConvExpr(ExprPtr v, TypePtr t)
        : Expr(AsConv), value(std::move(v)), target(std::move(t)) {}
};

struct OkExpr : Expr {
    ExprPtr value;
    explicit OkExpr(ExprPtr v) : Expr(Expr::OkExpr), value(std::move(v)) {}
};
struct ErExpr : Expr {
    ExprPtr value;
    explicit ErExpr(ExprPtr v) : Expr(Expr::ErExpr), value(std::move(v)) {}
};
struct TryExpr : Expr {
    ExprPtr value;
    explicit TryExpr(ExprPtr v) : Expr(Expr::TryExpr), value(std::move(v)) {}
};
struct UnwExpr : Expr {
    ExprPtr value, fallback;
    UnwExpr(ExprPtr v, ExprPtr f)
        : Expr(Expr::UnwExpr), value(std::move(v)), fallback(std::move(f)) {}
};
struct OrExpr : Expr {
    ExprPtr lhs, rhs;
    OrExpr(ExprPtr l, ExprPtr r)
        : Expr(Expr::OrExpr), lhs(std::move(l)), rhs(std::move(r)) {}
};

struct ArrayLitExpr : Expr {
    std::vector<ExprPtr> elems;
    ExprPtr repeatValue;
    ExprPtr repeatCount;
    bool    isRepeat = false;
    ArrayLitExpr() : Expr(ArrayLit) {}
};

struct TupleLitExpr : Expr {
    std::vector<ExprPtr> elems;
    TupleLitExpr() : Expr(TupleLit) {}
};

struct FieldInit {
    std::string name;
    ExprPtr     value;
    int         line = 1;
};

struct RecLitExpr : Expr {
    std::string typeName;
    std::vector<FieldInit> fields;
    std::vector<ExprPtr>   positionalArgs;
    explicit RecLitExpr(std::string n)
        : Expr(RecLit), typeName(std::move(n)) {}
};

struct ClsLitExpr : Expr {
    std::string typeName;
    std::vector<FieldInit> fields;
    std::vector<ExprPtr>   positionalArgs;
    explicit ClsLitExpr(std::string n)
        : Expr(ClsLit), typeName(std::move(n)) {}
};

struct EnmLitExpr : Expr {
    std::string typeName;
    std::string variant;
    std::vector<ExprPtr> args;
    EnmLitExpr(std::string t, std::string v)
        : Expr(EnmLit), typeName(std::move(t)), variant(std::move(v)) {}
};

struct RangeLitExpr : Expr {
    ExprPtr from, to, step;
    bool    exclusive = false;
    RangeLitExpr(ExprPtr f, ExprPtr t, bool ex)
        : Expr(RangeLit), from(std::move(f)),
          to(std::move(t)), exclusive(ex) {}
};

struct SizeofExpr : Expr {
    TypePtr operand;
    bool    isAlign = false;
    SizeofExpr(TypePtr t, bool a)
        : Expr(Expr::SizeofExpr), operand(std::move(t)), isAlign(a) {}
};

struct AddrExpr : Expr {
    ExprPtr target;
    explicit AddrExpr(ExprPtr t) : Expr(Expr::AddrExpr), target(std::move(t)) {}
};

struct DerefExpr : Expr {
    ExprPtr pointer;
    bool    isOwn = false;
    DerefExpr(ExprPtr p, bool own)
        : Expr(Expr::DerefExpr), pointer(std::move(p)), isOwn(own) {}
};

struct SelExpr : Expr {
    ExprPtr cond, thenV, elseV;
    SelExpr(ExprPtr c, ExprPtr t, ExprPtr e)
        : Expr(Expr::SelExpr), cond(std::move(c)),
          thenV(std::move(t)), elseV(std::move(e)) {}
};

struct BlkExpr : Expr {
    std::vector<StmtPtr> body;
    BlkExpr() : Expr(Expr::BlkExpr) {}
};

struct LambdaCapture {
    std::string innerName;
    std::string outerName;
};

struct LamExpr : Expr {
    std::vector<LambdaCapture> captures;
    std::vector<std::pair<std::string, TypePtr>> params;
    TypePtr              retType;
    std::vector<StmtPtr> body;
    ExprPtr              single;
    LamExpr() : Expr(Expr::LamExpr) {}
};

struct FmtPart {
    bool        isExpr = false;
    std::string text;
    ExprPtr     expr;
};

struct FmtExpr : Expr {
    std::vector<FmtPart> parts;
    FmtExpr() : Expr(Expr::FmtExpr) {}
};

struct PipeExpr : Expr {
    ExprPtr left;
    std::string fnName;
    std::vector<ExprPtr> args;
    PipeExpr(ExprPtr l, std::string n)
        : Expr(Expr::PipeExpr), left(std::move(l)), fnName(std::move(n)) {}
};

struct RseExpr : Expr {
    ExprPtr handle;
    explicit RseExpr(ExprPtr h) : Expr(Expr::RseExpr), handle(std::move(h)) {}
};

struct CteCallExpr : Expr {
    std::string fnName;
    std::vector<ExprPtr> args;
    explicit CteCallExpr(std::string n)
        : Expr(Expr::CteCall), fnName(std::move(n)) {}
};

// ----------------------------------------------------------------------------
// P4-2  模式
// ----------------------------------------------------------------------------
struct Pattern {
    enum K {
        Wild,
        Bind,
        LitInt, LitFlt, LitChr, LitStr, LitBol, LitNil,
        EnmPattern,
        RecPattern,
        TupPattern,
        Alias,
    } kind = Wild;

    std::string name;
    int64_t     ival = 0;
    double      fval = 0;
    char        cval = 0;
    std::string sval;
    bool        bval = false;

    std::string typeName;
    std::string variant;
    std::vector<std::shared_ptr<Pattern>> args;
    struct FieldPat {
        std::string name;
        std::shared_ptr<Pattern> pattern;
    };
    std::vector<FieldPat> fields;
    std::shared_ptr<Pattern> aliasInner;

    int line = 1;
};
using PatternPtr = std::shared_ptr<Pattern>;

// ----------------------------------------------------------------------------
// P4-3  语句
// ----------------------------------------------------------------------------
struct Stmt {
    enum K {
        VarDecl, Assign, MovAssign, SetAssign, IncDec, SwpStmt,
        ExprStmt,
        IfStmt, WhileStmt, ForRange, ForC,
        SwitchStmt,
        BreakStmt, ContStmt, JumpStmt, LabelStmt, RetStmt,
        PrintStmt, GetStmt,
        NewStmt, DelStmt,
        AtrStmt,
        AttrStmt,
        AsmStmt,
        UnwStmt,
        BlockStmt,
    } kind;
    int line = 1;
    int col  = 1;

    explicit Stmt(K k) : kind(k) {}
    virtual ~Stmt() = default;
};

struct VarDeclStmt : Stmt {
    struct Item {
        std::string name;
        TypePtr     type;
        bool        isReg = false;
        std::string ownerModule;
        int         slot = -1;
    };
    std::vector<Item> vars;
    VarDeclStmt() : Stmt(VarDecl) {}
};

struct AssignStmt : Stmt {
    ExprPtr target, value;
    AssignStmt(ExprPtr t, ExprPtr v)
        : Stmt(Assign), target(std::move(t)), value(std::move(v)) {}
};

struct MovAssignStmt : Stmt {
    ExprPtr target, value;
    MovAssignStmt(ExprPtr t, ExprPtr v)
        : Stmt(MovAssign), target(std::move(t)), value(std::move(v)) {}
};

struct SetAssignStmt : Stmt {
    ExprPtr target, value;
    SetAssignStmt(ExprPtr t, ExprPtr v)
        : Stmt(SetAssign), target(std::move(t)), value(std::move(v)) {}
};

struct IncDecStmt : Stmt {
    ExprPtr target;
    bool    isInc = true;
    IncDecStmt(ExprPtr t, bool inc)
        : Stmt(IncDec), target(std::move(t)), isInc(inc) {}
};

struct SwpStmt : Stmt {
    ExprPtr lhs, rhs;
    SwpStmt(ExprPtr l, ExprPtr r)
        : Stmt(Stmt::SwpStmt), lhs(std::move(l)), rhs(std::move(r)) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    explicit ExprStmt(ExprPtr e) : Stmt(Stmt::ExprStmt), expr(std::move(e)) {}
};

struct IfStmt : Stmt {
    ExprPtr cond;
    std::vector<StmtPtr> thenBody;
    std::vector<StmtPtr> elseBody;
    bool    hasElse = false;
    IfStmt() : Stmt(Stmt::IfStmt) {}
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    std::vector<StmtPtr> body;
    std::string label;
    WhileStmt() : Stmt(Stmt::WhileStmt) {}
};

struct ForRangeStmt : Stmt {
    std::string varName;
    ExprPtr from, to, step;
    bool    exclusive = false;
    bool    isForEach = false;
    std::vector<StmtPtr> body;
    std::string label;
    ForRangeStmt() : Stmt(ForRange) {}
};

struct ForCStmt : Stmt {
    StmtPtr init;
    ExprPtr cond;
    StmtPtr incr;
    std::vector<StmtPtr> body;
    std::string label;
    ForCStmt() : Stmt(ForC) {}
};

struct CaseClause {
    PatternPtr pattern;
    ExprPtr    guard;
    std::vector<StmtPtr> body;
    int line = 1;
};

struct SwitchStmt : Stmt {
    ExprPtr subject;
    std::vector<CaseClause> cases;
    std::vector<StmtPtr> defBody;
    bool    hasDefault = false;
    std::string label;
    SwitchStmt() : Stmt(Stmt::SwitchStmt) {}
};

struct BreakStmt : Stmt {
    std::string label;
    BreakStmt() : Stmt(Stmt::BreakStmt) {}
};
struct ContStmt : Stmt {
    std::string label;
    ContStmt() : Stmt(Stmt::ContStmt) {}
};
struct JumpStmt : Stmt {
    std::string label;
    JumpStmt() : Stmt(Stmt::JumpStmt) {}
};
struct LabelStmt : Stmt {
    std::string name;
    LabelStmt() : Stmt(Stmt::LabelStmt) {}
};
struct RetStmt : Stmt {
    ExprPtr value;
    RetStmt() : Stmt(Stmt::RetStmt) {}
};

struct PrintStmt : Stmt {
    enum Stream { Stdout, Stderr } stream = Stdout;
    bool    newline = false;
    std::vector<ExprPtr> exprs;
    PrintStmt() : Stmt(Stmt::PrintStmt) {}
};

struct GetStmt : Stmt {
    std::vector<ExprPtr> targets;
    GetStmt() : Stmt(Stmt::GetStmt) {}
};

struct NewStmt : Stmt {
    std::string varName;
    NewStmt() : Stmt(Stmt::NewStmt) {}
};
struct DelStmt : Stmt {
    std::string varName;
    DelStmt() : Stmt(Stmt::DelStmt) {}
};

struct AtrStmt : Stmt {
    ExprPtr value;
    explicit AtrStmt(ExprPtr v) : Stmt(Stmt::AtrStmt), value(std::move(v)) {}
};

struct AttrStmt : Stmt {
    std::string varName;
    std::string attrName;
    ExprPtr     arg;
    AttrStmt() : Stmt(Stmt::AttrStmt) {}
};

struct AsmStmt : Stmt {
    std::string text;
    AsmStmt() : Stmt(Stmt::AsmStmt) {}
};

struct UnwStmt : Stmt {
    std::string varName;
    ExprPtr     expr;
    ExprPtr     fallback;
    UnwStmt() : Stmt(Stmt::UnwStmt) {}
};

struct BlockStmt : Stmt {
    std::vector<StmtPtr> body;
    BlockStmt() : Stmt(Stmt::BlockStmt) {}
};

// ----------------------------------------------------------------------------
// P4-4  声明
// ----------------------------------------------------------------------------
struct Param {
    std::string name;
    TypePtr     type;
    ExprPtr     defaultValue;
    bool        hasDefault = false;
    int         line = 1;
};

struct Function {
    std::string name;
    std::vector<Param> params;
    TypePtr              retType;
    std::vector<StmtPtr> body;
    bool hasBody        = false;
    bool isEntry        = false;
    bool isStatic       = false;
    bool isVirtual      = false;
    bool isAbstract     = false;
    bool isFinal        = false;
    bool isInline       = false;
    bool isCompileTime  = false;
    bool isCoroutine    = false;

    std::vector<std::string> genericParams;
    std::vector<std::string> genericConstraints;

    std::string ownerClass;
    std::string ownerModule;

    int localSize  = 0;
    int frameSlots = 0;
    std::vector<int> paramSlots;

    int line = 1;
};

struct RecordDecl {
    std::string name;
    std::vector<std::string> genericParams;
    std::vector<VarDeclStmt::Item> fields;
    int line = 1;
};
using RecordPtr = std::shared_ptr<RecordDecl>;

struct EnumVariant {
    std::string name;
    std::vector<TypePtr> payloadTypes;
    int line = 1;
};
struct EnumDecl {
    std::string name;
    std::vector<std::string> genericParams;
    std::vector<EnumVariant> variants;
    int line = 1;
};
using EnumPtr = std::shared_ptr<EnumDecl>;

struct UnionDecl {
    std::string name;
    std::vector<VarDeclStmt::Item> fields;
    int line = 1;
};
using UnionPtr = std::shared_ptr<UnionDecl>;

struct SigDecl {
    std::string name;
    std::vector<TypePtr> paramTypes;
    TypePtr retType;
    int line = 1;
};

struct TraitDecl {
    std::string name;
    std::string forTypeName;
    std::vector<FunctionPtr> methods;
    int line = 1;
};
using TraitPtr = std::shared_ptr<TraitDecl>;

struct TypDef {
    std::string name;
    std::vector<std::string> genericParams;
    TypePtr definition;
    int line = 1;
};

struct ClassDecl;
using ClassPtr = std::shared_ptr<ClassDecl>;

struct ClassMember {
    enum K {
        Field,
        Method,
        AbstractMethod,
        StaticMethod,
        VirtualMethod,
        FinalMethod,
        Ctor,
        Dtor,
        FriendFun,
        FriendCls,
        NestedClass,
    } kind = Field;

    enum Access { Public, Protected, Private } access = Private;

    VarDeclStmt::Item field;
    FunctionPtr      method;
    ClassPtr         nestedClass;
    std::string      friendName;
    int line = 1;
};

struct ClassDecl {
    std::string name;
    std::vector<std::string> genericParams;
    std::string superClass;
    std::vector<ClassMember> members;
    bool isFinal = false;
    int line = 1;
};

struct ModuleDecl;
using ModulePtr = std::shared_ptr<ModuleDecl>;
struct ModuleDecl {
    std::string name;
    std::vector<std::string> members;
    std::unordered_set<std::string> exported;
    int line = 1;
};

struct SextStmt {
    enum K {
        Rep, If, For,
        Set, Equ,
        Lod, Upd,
        Inc,
        Alc,
        Gen,
        DefAttr, DefMac,
        CteCall,
    } kind = Gen;

    std::string name;
    ExprPtr     expr;
    std::string text;
    ExprPtr     arg1;
    ExprPtr     arg2;
    std::vector<ExprPtr> cteArgs;
    PatternPtr  macroPattern;
    std::vector<SextStmt> body;
    std::vector<SextStmt> elseBody;
    int line = 1;
};

struct SextBlock {
    std::vector<SextStmt> stmts;
    int line = 1;
};
using SextBlockPtr = std::shared_ptr<SextBlock>;

// ----------------------------------------------------------------------------
// P4-5  TopLevel 与 Program
// ----------------------------------------------------------------------------
struct TopLevel {
    enum K {
        Empty,
        Fun, Rec, Enm, Uni, Sig, Tra, Cls, Mod,
        Alias,
        Equ,
        GlobalVar,
        Sext,
        Use, Exp, Ext,
        Pkg, Lod, Upd,
        Attr,
    } kind = Empty;

    FunctionPtr  fun;
    RecordPtr    rec;
    EnumPtr      enm;
    UnionPtr     uni;
    std::shared_ptr<SigDecl>  sig;
    TraitPtr     tra;
    ClassPtr     cls;
    ModulePtr    mod;
    std::shared_ptr<TypDef>   typ;
    SextBlockPtr sext;
    StmtPtr      stmt;

    std::string s1;
    std::string s2;

    int line = 1;
};

struct Program {
    std::vector<TopLevel> topLevel;

    std::unordered_map<std::string, FunctionPtr> functions;
    std::unordered_map<std::string, FunctionPtr> compileTimeFunctions;
    std::unordered_map<std::string, RecordPtr>   records;
    std::unordered_map<std::string, EnumPtr>     enums;
    std::unordered_map<std::string, UnionPtr>    unions;
    std::unordered_map<std::string, ClassPtr>    classes;
    std::unordered_map<std::string, TraitPtr>    traits;
    std::unordered_map<std::string, std::vector<TraitPtr>> traitsByType;
    std::unordered_map<std::string, std::shared_ptr<SigDecl>> sigs;
    std::unordered_map<std::string, ModulePtr>   modules;
    std::unordered_map<std::string, std::shared_ptr<TypDef>> typedefs;
    std::unordered_map<std::string, int64_t>     consts;

    std::unordered_map<std::string, std::string> useAliases;

    std::vector<VarDeclStmt::Item> globals;
    std::vector<int>               globalSlots;

    FunctionPtr entry;
};

// P4 结束。namespace arc 保持打开。
// ============================================================================
// P5  语法解析器（上）—— 骨架、顶层分发、类型、声明
// ============================================================================

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    std::shared_ptr<Program> parseProgram();

private:
    std::vector<Token> toks_;
    size_t             i_ = 0;
    std::vector<std::string> moduleStack_;
    std::unordered_map<std::string, int64_t> consts_;
    Program*           curProg_ = nullptr;

    const Token& peek() const { return toks_[i_]; }
    const Token& peekAt(size_t off) const {
        size_t p = i_ + off;
        return p < toks_.size() ? toks_[p] : toks_.back();
    }
    const Token& adv() { return toks_[i_++]; }
    bool check(TT t) const { return peek().type == t; }
    bool match(TT t) {
        if (check(t)) { ++i_; return true; }
        return false;
    }
    bool checkIdent(const std::string& s) const {
        return check(TT::Ident) && peek().text == s;
    }
    const Token& expect(TT t, const char* what) {
        if (!check(t)) {
            std::ostringstream ss;
            ss << "expected " << what << ", got '" << peek().text << "'";
            throw ArcError::at(peek().line, peek().col, ss.str());
        }
        return adv();
    }
    const Token& expectIdent(const char* what) {
        return expect(TT::Ident, what);
    }
    [[noreturn]] void fail(const std::string& msg) const {
        throw ArcError::at(peek().line, peek().col, msg);
    }

    void skipNewlines() { while (check(TT::Newline)) adv(); }

    bool lookaheadIsIndentBlock(size_t off = 0) const {
        return peekAt(off).type == TT::Newline &&
               peekAt(off + 1).type == TT::Indent;
    }

    // ---------------- 顶层 ----------------
    TopLevel parseTopLevel(Program& prog);
    TopLevel parseFunction(Program& prog, bool isCte = false);
    TopLevel parseEntry(Program& prog);
    TopLevel parseCoroutine(Program& prog);
    TopLevel parseRecordDef(Program& prog);
    TopLevel parseEnumDef(Program& prog);
    TopLevel parseUnionDef(Program& prog);
    TopLevel parseSigDef(Program& prog);
    TopLevel parseTraitDef(Program& prog);
    TopLevel parseTypeAlias(Program& prog);
    TopLevel parseClassDef(Program& prog, bool isFinal);
    TopLevel parseModuleDef(Program& prog);
    TopLevel parseSextBlock(Program& prog);
    TopLevel parseUseStmt(Program& prog);
    TopLevel parsePkgStmt(Program& prog);
    TopLevel parseLodStmt(Program& prog);
    TopLevel parseUpdStmt(Program& prog);
    TopLevel parseExpDecl(Program& prog);
    TopLevel parseExtDecl(Program& prog);
    TopLevel parseEquStmt(Program& prog);
    TopLevel parseGlobalVarDecl(Program& prog);
    TopLevel parseAttrTopLevel(Program& prog);

    // ---------------- 类型 ----------------
    TypePtr parseType();
    TypePtr parseTypeBase();
    std::vector<std::string> parseGenericParams();
    std::vector<TypePtr>     parseGenericArgs();
    std::vector<Param>       parseParamList();

    // ---------------- 类 ----------------
    void parseClassBody(ClassDecl& cls);
    void parseAccessBlock(ClassDecl& cls, ClassMember::Access acc);
    void parseClassMember(ClassDecl& cls, ClassMember::Access acc);

    // ---------------- 类验证（批次 2.5） ----------------
    void validateClasses(Program& prog);

    // ---------------- sext ----------------
    void parseSextSuite(std::vector<SextStmt>& out);
    SextStmt parseSextStmt();

    // ---------------- 批次 4：cte + sext ----------------
    std::optional<int64_t> tryEvalConst(Expr* e, Program& prog);
    std::optional<int64_t> evalCteCall(Function* fn,
                                       std::vector<int64_t> args,
                                       Program& prog);
    std::optional<int64_t> lookupConst(const std::string& name) const {
        auto it = consts_.find(name);
        if (it == consts_.end()) return std::nullopt;
        return it->second;
    }
    void execSextBlock(SextBlock* blk, Program& prog);
    void execSextStmt(SextStmt& s, Program& prog);

    // ---------------- 语句（P6 实现） ----------------
    std::vector<StmtPtr> parseSuite();
    StmtPtr parseStmt();
    StmtPtr parseVarDeclStmt(bool isReg = false);
    StmtPtr parseIfStmt();
    StmtPtr parseWhileStmt();
    StmtPtr parseForStmt();
    StmtPtr parseSwitchStmt();
    StmtPtr parseRetStmt();
    StmtPtr parsePrintStmt();
    StmtPtr parseGetStmt();
    StmtPtr parseNewStmt();
    StmtPtr parseDelStmt();
    StmtPtr parseAsmStmt();
    StmtPtr parseUnwStmt();
    StmtPtr parseAtrStmt();
    StmtPtr parseIncDecStmt(bool isInc);
    StmtPtr parseMovStmt();
    StmtPtr parseSetStmt();
    StmtPtr parseSwpStmt();
    StmtPtr parseAttrStmt();
    PatternPtr parsePattern();

    // ---------------- 表达式（P7 实现） ----------------
    ExprPtr parseExpr();
    ExprPtr parsePipeExpr();
    ExprPtr parseOrExpr();
    ExprPtr parseAndExpr();
    ExprPtr parseEqualityExpr();
    ExprPtr parseRelationalExpr();
    ExprPtr parseAdditiveExpr();
    ExprPtr parseMultiplicativeExpr();
    ExprPtr parsePowerExpr();
    ExprPtr parseUnaryExpr();
    ExprPtr parsePostfixExpr();
    ExprPtr parsePrimaryExpr();
    ExprPtr parseArrayLit();
    ExprPtr parseTupleLit();
    ExprPtr parseRecLit();
    ExprPtr parseClsLit();
    ExprPtr parseEnmLit();
    ExprPtr parseRangeLit();
    ExprPtr parseLambda();
    ExprPtr parseFmtExpr();
    ExprPtr parseBlkExpr();
    ExprPtr parseSelExpr();
    ExprPtr parseNamedInstr(const std::string& name, int line, int col);

    void instantiateIfGeneric(const std::string& mangled);
};

// ----------------------------------------------------------------------------
// P5-A  顶层入口
// ----------------------------------------------------------------------------
inline std::shared_ptr<Program> Parser::parseProgram() {
    auto prog = std::make_shared<Program>();
    curProg_ = prog.get();
    skipNewlines();
    while (!check(TT::Eof)) {
        TopLevel tl = parseTopLevel(*prog);
        if (tl.kind != TopLevel::Empty)
            prog->topLevel.push_back(std::move(tl));
        skipNewlines();
    }
    for (auto& kv : consts_)
        prog->consts[kv.first] = kv.second;

    validateClasses(*prog);
    return prog;
}

inline TopLevel Parser::parseTopLevel(Program& prog) {
    const Token& t = peek();

    if (t.type == TT::Ident && t.text == "attr")
        return parseAttrTopLevel(prog);

    switch (t.type) {
        case TT::Kw_fun:  return parseFunction(prog, false);
        case TT::Kw_cte: {
            if (peekAt(1).type == TT::Kw_fun) return parseFunction(prog, true);
            fail("'cte' must be followed by 'fun' at top level");
        }
        case TT::Kw_ent:  return parseEntry(prog);
        case TT::Kw_cor:  return parseCoroutine(prog);
        case TT::Kw_rec:  return parseRecordDef(prog);
        case TT::Kw_enm:  return parseEnumDef(prog);
        case TT::Kw_uni:  return parseUnionDef(prog);
        case TT::Kw_sig:  return parseSigDef(prog);
        case TT::Kw_tra:  return parseTraitDef(prog);
        case TT::Kw_typ:  return parseTypeAlias(prog);
        case TT::Kw_cls:  return parseClassDef(prog, false);
        case TT::Kw_fnl:  return parseClassDef(prog, true);
        case TT::Kw_mod:  return parseModuleDef(prog);
        case TT::Kw_sext: return parseSextBlock(prog);
        case TT::Kw_use:  return parseUseStmt(prog);
        case TT::Kw_pkg:  return parsePkgStmt(prog);
        case TT::Kw_lod:  return parseLodStmt(prog);
        case TT::Kw_upd:  return parseUpdStmt(prog);
        case TT::Kw_exp:  return parseExpDecl(prog);
        case TT::Kw_ext:  return parseExtDecl(prog);
        case TT::Kw_equ:  return parseEquStmt(prog);
        case TT::Kw_var:  return parseGlobalVarDecl(prog);
        default: break;
    }
    fail("unexpected top-level token '" + t.text + "'");
}

// ----------------------------------------------------------------------------
// P5-B  类型
// ----------------------------------------------------------------------------
inline TypePtr substTypeGeneric(
        const TypePtr& t,
        const std::unordered_map<std::string, TypePtr>& subst) {
    if (!t) return t;
    if (t->kind == Type::Named) {
        auto it = subst.find(t->name);
        if (it != subst.end()) return it->second;
    }
    if (t->args.empty()) return t;
    auto nt = std::make_shared<Type>(*t);
    for (auto& a : nt->args) a = substTypeGeneric(a, subst);
    return nt;
}

inline void Parser::instantiateIfGeneric(const std::string& mangled) {
    if (!curProg_) return;
    auto dollar = mangled.find('$');
    if (dollar == std::string::npos) return;
    if (curProg_->classes.count(mangled)) return;

    std::string base = mangled.substr(0, dollar);
    auto tmplIt = curProg_->classes.find(base);
    if (tmplIt == curProg_->classes.end()) return;
    auto tmpl = tmplIt->second;
    if (!tmpl || tmpl->genericParams.empty()) return;

    std::vector<std::string> argNames;
    std::string cur;
    size_t i = dollar + 1;
    int depth = 0;
    while (i < mangled.size()) {
        char c = mangled[i];
        if (c == '$' && depth == 0) {
            argNames.push_back(cur);
            cur.clear();
        } else {
            if (c == '<') ++depth;
            else if (c == '>') --depth;
            cur += c;
        }
        ++i;
    }
    if (!cur.empty()) argNames.push_back(cur);

    if (argNames.size() != tmpl->genericParams.size()) return;

    std::unordered_map<std::string, TypePtr> subst;
    for (size_t k = 0; k < argNames.size(); ++k) {
        subst[tmpl->genericParams[k]] = Type::named(argNames[k]);
    }

    auto inst = std::make_shared<ClassDecl>(*tmpl);
    inst->name = mangled;
    inst->genericParams.clear();
    for (auto& m : inst->members) {
        m.field.type = substTypeGeneric(m.field.type, subst);
        // 不深拷贝方法：解释器不检查参数/返回类型，共享模板方法即可。
        // （深拷贝需要 Function 显式拷贝构造，因为 body 是 vector<unique_ptr>）
    }
    curProg_->classes[mangled] = inst;
}

inline TypePtr Parser::parseTypeBase() {

    if (match(TT::Star))  return Type::makePtr(parseType());
    if (match(TT::Caret)) return Type::makeOwnPtr(parseType());

    if (check(TT::LParen)) {
        adv();
        std::vector<TypePtr> elems;
        if (!check(TT::RParen)) {
            elems.push_back(parseType());
            while (match(TT::Comma)) elems.push_back(parseType());
        }
        expect(TT::RParen, "')'");
        if (match(TT::Arrow)) {
            TypePtr ret = parseType();
            return Type::makeFun(std::move(elems), std::move(ret));
        }
        if (elems.size() == 1) return elems[0];
        fail("parenthesized type list requires '->'");
    }

    if (check(TT::Kw_int)) { adv(); return Type::base(Type::Int); }
    if (check(TT::Kw_flt)) { adv(); return Type::base(Type::Flt); }
    if (check(TT::Kw_chr)) { adv(); return Type::base(Type::Chr); }
    if (check(TT::Kw_bol)) { adv(); return Type::base(Type::Bol); }
    if (check(TT::Kw_str)) { adv(); return Type::base(Type::Str); }
    if (check(TT::Kw_nil)) { adv(); return Type::base(Type::Nil); }
    if (check(TT::Kw_cod)) { adv(); return Type::base(Type::Cod); }

    if (check(TT::Kw_arr)) {
        adv();
        expect(TT::Lt, "'<'");
        TypePtr elem = parseType();
        int64_t n = 0;
        if (match(TT::Comma)) {
            if (check(TT::IntLit)) {
                n = adv().ival;
            } else if (check(TT::Ident)) {
                std::string cname = adv().text;
                auto it = consts_.find(cname);
                if (it != consts_.end()) n = it->second;
            } else {
                fail("array size must be an integer literal or constant");
            }
        }
        expect(TT::Gt, ">");
        return Type::makeArr(std::move(elem), n);
    }

    if (check(TT::Kw_map)) {
        adv();
        expect(TT::Lt, "'<'");
        TypePtr k = parseType();
        expect(TT::Comma, "','");
        TypePtr v = parseType();
        expect(TT::Gt, "'>'");
        return Type::makeMap(std::move(k), std::move(v));
    }

    if (check(TT::Kw_opt)) {
        adv();
        expect(TT::Lt, "'<'");
        TypePtr inner = parseType();
        expect(TT::Gt, "'>'");
        return Type::makeOpt(std::move(inner));
    }

    if (check(TT::Kw_sig)) {
        adv();
        std::string name = expectIdent("signature name").text;
        return Type::makeSig(name);
    }

    if (check(TT::Kw_rng)) {
        adv();
        expect(TT::Lt, "'<'");
        TypePtr inner = parseType();
        expect(TT::Gt, "'>'");
        return Type::makeRng(std::move(inner));
    }

    if (check(TT::Kw_slc)) {
        adv();
        expect(TT::Lt, "'<'");
        TypePtr inner = parseType();
        expect(TT::Gt, "'>'");
        return Type::makeSlc(std::move(inner));
    }

    if (check(TT::Kw_tup)) {
        adv();
        expect(TT::Lt, "'<'");
        std::vector<TypePtr> elems;
        elems.push_back(parseType());
        while (match(TT::Comma)) elems.push_back(parseType());
        expect(TT::Gt, "'>'");
        return Type::makeTup(std::move(elems));
    }

    if (check(TT::Kw_tck)) {
        adv();
        expect(TT::Lt, "'<'");
        TypePtr inner = parseType();
        expect(TT::Gt, "'>'");
        return Type::makeTck(std::move(inner));
    }

    if (check(TT::Kw_rlt)) {
        adv();
        expect(TT::Lt, "'<'");
        TypePtr ok = parseType();
        expect(TT::Comma, "','");
        TypePtr err = parseType();
        expect(TT::Gt, "'>'");
        return Type::makeRlt(std::move(ok), std::move(err));
    }

    if (check(TT::Ident)) {
        std::string name = adv().text;
        if (check(TT::Lt)) {
            adv();
            std::vector<TypePtr> args;
            args.push_back(parseType());
            while (match(TT::Comma)) args.push_back(parseType());
            expect(TT::Gt, "'>'");
            std::string mangled = name;
            for (auto& a : args) mangled += "$" + a->toString();
            instantiateIfGeneric(mangled);
            auto t = Type::named(mangled);
            t->args = args;
            return t;
        }
        if (curProg_) {
            auto it = curProg_->typedefs.find(name);
            if (it != curProg_->typedefs.end() &&
                it->second && it->second->definition) {
                return it->second->definition;
            }
        }
        return Type::named(name);
    }

    fail("expected a type");
}

inline TypePtr Parser::parseType() { return parseTypeBase(); }

inline std::vector<std::string> Parser::parseGenericParams() {
    std::vector<std::string> out;
    if (!match(TT::Lt)) return out;
    if (!check(TT::Gt)) {
        out.push_back(expectIdent("generic parameter").text);
        while (match(TT::Comma)) {
            out.push_back(expectIdent("generic parameter").text);
        }
    }
    expect(TT::Gt, "'>'");
    return out;
}

inline std::vector<TypePtr> Parser::parseGenericArgs() {
    std::vector<TypePtr> out;
    if (!match(TT::Lt)) return out;
    if (!check(TT::Gt)) {
        out.push_back(parseType());
        while (match(TT::Comma)) out.push_back(parseType());
    }
    expect(TT::Gt, "'>'");
    return out;
}

inline std::vector<Param> Parser::parseParamList() {
    std::vector<Param> out;
    if (check(TT::RParen)) return out;

    do {
        Param p;
        p.line = peek().line;

        if (checkIdent("self")) {
            adv();
            p.name = "self";
            p.type = parseType();
            out.push_back(std::move(p));
            continue;
        }

        p.name = expectIdent("parameter name").text;
        expect(TT::Colon, "':' before parameter type");
        p.type = parseType();
        if (match(TT::Assign)) {
            p.hasDefault  = true;
            p.defaultValue = parseExpr();
        }
        out.push_back(std::move(p));
    } while (match(TT::Comma));

    return out;
}

// ----------------------------------------------------------------------------
// P5-C  函数 / 入口 / 协程 / cte fun
// ----------------------------------------------------------------------------
inline TopLevel Parser::parseFunction(Program& prog, bool isCte) {
    int ln = peek().line;
    int cl = peek().col;

    if (isCte) expect(TT::Kw_cte, "'cte'");
    expect(TT::Kw_fun, "'fun'");

    std::string name = expectIdent("function name").text;

    auto& registry = isCte ? prog.compileTimeFunctions : prog.functions;
    auto fn = registry.count(name) ? registry[name]
                                   : std::make_shared<Function>();
    fn->name = name;
    fn->line = ln;
    fn->isCompileTime = isCte;

    if (check(TT::Lt)) fn->genericParams = parseGenericParams();

    while (check(TT::Kw_whe)) {
        adv();
        std::string param = expectIdent("generic param").text;
        expect(TT::Colon, "':'");
        std::string trait = expectIdent("trait name").text;
        fn->genericConstraints.push_back(param + ":" + trait);
        if (!match(TT::Comma)) break;
    }

    bool hadParamsBefore = !fn->params.empty();
    if (check(TT::LParen)) {
        adv();
        auto params = parseParamList();
        if (!params.empty()) {
            if (hadParamsBefore) {
                if (params.size() != fn->params.size()) {
                    fail("function '" + name + "' signature mismatch: "
                         "previously " + std::to_string(fn->params.size()) +
                         " params, now " + std::to_string(params.size()));
                }
                for (size_t pi = 0; pi < params.size(); ++pi) {
                    std::string pT = params[pi].type ? params[pi].type->toString() : "?";
                    std::string qT = fn->params[pi].type ? fn->params[pi].type->toString() : "?";
                    if (pT != qT) {
                        fail("function '" + name + "' parameter " +
                             std::to_string(pi) + " type mismatch: previously '" +
                             qT + "', now '" + pT + "'");
                    }
                }
            }
            fn->params = std::move(params);
        }
        expect(TT::RParen, "')'");
    }

    if (match(TT::FatArrow)) {
        auto e = parseExpr();
        auto r = std::make_unique<RetStmt>();
        r->line  = e->line;
        r->col   = e->col;
        r->value = std::move(e);
        fn->body.push_back(std::move(r));
        fn->hasBody = true;
    } else if (match(TT::Colon)) {
        bool hasRetType = !check(TT::Newline) && !check(TT::FatArrow) &&
                          !check(TT::Colon) && tokenStartsType(peek().type);
        if (hasRetType) fn->retType = parseType();

        if (match(TT::Colon)) {
            fn->body    = parseSuite();
            fn->hasBody = true;
        } else if (match(TT::FatArrow)) {
            auto e = parseExpr();
            auto r = std::make_unique<RetStmt>();
            r->line  = e->line;
            r->col   = e->col;
            r->value = std::move(e);
            fn->body.push_back(std::move(r));
            fn->hasBody = true;
        } else if (!hasRetType && check(TT::Newline)) {
            fn->body    = parseSuite();
            fn->hasBody = true;
        }
    } else {
        if (lookaheadIsIndentBlock(0))
            fail("expected ':' after function signature");
    }

    std::string regName = name;
    if (!isCte && !moduleStack_.empty()) {
        regName = moduleStack_.back() + "$" + name;
        fn->ownerModule = moduleStack_.back();
        auto mit = prog.modules.find(moduleStack_.back());
        if (mit != prog.modules.end() && mit->second)
            mit->second->members.push_back(regName);
    }
    if (isCte) prog.compileTimeFunctions[regName] = fn;
    else       prog.functions[regName] = fn;

    TopLevel tl;
    tl.kind = TopLevel::Fun;
    tl.fun  = fn;
    tl.line = ln;
    return tl;
}

inline TopLevel Parser::parseEntry(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_ent, "'ent'");
    std::string name = expectIdent("entry name").text;

    auto fn = std::make_shared<Function>();
    fn->name    = name;
    fn->isEntry = true;
    fn->line    = ln;

    if (check(TT::LParen)) {
        adv();
        fn->params = parseParamList();
        expect(TT::RParen, "')'");
    }

    if (match(TT::FatArrow)) {
        auto e = parseExpr();
        auto r = std::make_unique<RetStmt>();
        r->line  = e->line;
        r->value = std::move(e);
        fn->body.push_back(std::move(r));
        fn->hasBody = true;
    } else if (match(TT::Colon)) {
        bool hasRetType = !check(TT::Newline) && !check(TT::FatArrow) &&
                          tokenStartsType(peek().type);
        if (hasRetType) {
            fn->retType = parseType();
            match(TT::Colon);
        }
        fn->body    = parseSuite();
        fn->hasBody = true;
    } else {
        fail("expected ':' or '=>' after entry declaration");
    }

    if (!fn->retType) fn->retType = Type::base(Type::Int);

    prog.entry            = fn;
    prog.functions[name]  = fn;

    TopLevel tl;
    tl.kind = TopLevel::Fun;
    tl.fun  = fn;
    tl.line = ln;
    return tl;
}

inline TopLevel Parser::parseCoroutine(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_cor, "'cor'");
    std::string name = expectIdent("coroutine name").text;

    auto fn = std::make_shared<Function>();
    fn->name        = name;
    fn->isCoroutine = true;
    fn->line        = ln;

    if (check(TT::LParen)) {
        adv();
        fn->params = parseParamList();
        expect(TT::RParen, "')'");
    }

    expect(TT::Colon, "':'");
    fn->retType = parseType();
    expect(TT::Colon, "':'");
    fn->body    = parseSuite();
    fn->hasBody = true;

    prog.functions[name] = fn;

    TopLevel tl;
    tl.kind = TopLevel::Fun;
    tl.fun  = fn;
    tl.line = ln;
    return tl;
}

// ----------------------------------------------------------------------------
// P5-D  rec / enm / uni / sig
// ----------------------------------------------------------------------------
inline TopLevel Parser::parseRecordDef(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_rec, "'rec'");
    std::string name = expectIdent("record name").text;

    auto rec = std::make_shared<RecordDecl>();
    rec->name = name;
    rec->line = ln;

    if (check(TT::Lt)) rec->genericParams = parseGenericParams();
    expect(TT::Colon, "':'");
    expect(TT::Newline, "newline after rec header");
    expect(TT::Indent, "indented body");

    while (!check(TT::Dedent) && !check(TT::Eof)) {
        if (check(TT::Kw_var)) {
            adv();
            do {
                VarDeclStmt::Item it;
                it.name = expectIdent("field name").text;
                expect(TT::Colon, "':'");
                it.type = parseType();
                rec->fields.push_back(std::move(it));
            } while (match(TT::Comma));
        } else {
            fail("expected 'var' inside record body");
        }
        skipNewlines();
    }
    expect(TT::Dedent, "dedent");

    prog.records[name] = rec;

    TopLevel tl;
    tl.kind = TopLevel::Rec;
    tl.rec  = rec;
    tl.line = ln;
    return tl;
}

inline TopLevel Parser::parseEnumDef(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_enm, "'enm'");
    std::string name = expectIdent("enum name").text;

    auto en = std::make_shared<EnumDecl>();
    en->name = name;
    en->line = ln;

    if (check(TT::Lt)) en->genericParams = parseGenericParams();
    expect(TT::Colon, "':'");
    expect(TT::Newline, "newline after enm header");
    expect(TT::Indent, "indented body");

    while (!check(TT::Dedent) && !check(TT::Eof)) {
        expect(TT::Kw_cas, "'cas'");
        EnumVariant v;
        v.line = peek().line;
        v.name = expectIdent("variant name").text;
        if (match(TT::Colon)) {
            v.payloadTypes.push_back(parseType());
            while (match(TT::Comma)) v.payloadTypes.push_back(parseType());
        }
        en->variants.push_back(std::move(v));
        skipNewlines();
    }
    expect(TT::Dedent, "dedent");

    prog.enums[name] = en;

    TopLevel tl;
    tl.kind = TopLevel::Enm;
    tl.enm  = en;
    tl.line = ln;
    return tl;
}

inline TopLevel Parser::parseUnionDef(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_uni, "'uni'");
    std::string name = expectIdent("union name").text;

    auto un = std::make_shared<UnionDecl>();
    un->name = name;
    un->line = ln;

    expect(TT::Colon, "':'");
    expect(TT::Newline, "newline after uni header");
    expect(TT::Indent, "indented body");

    while (!check(TT::Dedent) && !check(TT::Eof)) {
        if (check(TT::Kw_var)) {
            adv();
            do {
                VarDeclStmt::Item it;
                it.name = expectIdent("field name").text;
                expect(TT::Colon, "':'");
                it.type = parseType();
                un->fields.push_back(std::move(it));
            } while (match(TT::Comma));
        } else {
            fail("expected 'var' inside union body");
        }
        skipNewlines();
    }
    expect(TT::Dedent, "dedent");

    prog.unions[name] = un;

    TopLevel tl;
    tl.kind = TopLevel::Uni;
    tl.uni  = un;
    tl.line = ln;
    return tl;
}

inline TopLevel Parser::parseSigDef(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_sig, "'sig'");
    std::string name = expectIdent("signature name").text;

    auto sig = std::make_shared<SigDecl>();
    sig->name = name;
    sig->line = ln;

    if (check(TT::LParen)) {
        adv();
        if (!check(TT::RParen)) {
            sig->paramTypes.push_back(parseType());
            while (match(TT::Comma)) sig->paramTypes.push_back(parseType());
        }
        expect(TT::RParen, "')'");
    }
    if (match(TT::Colon)) sig->retType = parseType();

    prog.sigs[name] = sig;

    TopLevel tl;
    tl.kind = TopLevel::Sig;
    tl.sig  = sig;
    tl.line = ln;
    return tl;
}

// P5-A/B/C/D 结束。
// ============================================================================
// P5-E  tra / typ
// ============================================================================
inline TopLevel Parser::parseTraitDef(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_tra, "'tra'");
    std::string name = expectIdent("trait name").text;
    expect(TT::Kw_for, "'for'");
    std::string forType;
    if      (check(TT::Ident))  forType = adv().text;
    else if (check(TT::Kw_int)) { adv(); forType = "int"; }
    else if (check(TT::Kw_flt)) { adv(); forType = "flt"; }
    else if (check(TT::Kw_chr)) { adv(); forType = "chr"; }
    else if (check(TT::Kw_bol)) { adv(); forType = "bol"; }
    else if (check(TT::Kw_str)) { adv(); forType = "str"; }
    else if (check(TT::Kw_nil)) { adv(); forType = "nil"; }
    else fail("expected target type name after 'for'");

    auto tra = std::make_shared<TraitDecl>();
    tra->name        = name;
    tra->forTypeName = forType;
    tra->line        = ln;

    expect(TT::Colon, "':'");
    expect(TT::Newline, "newline after tra header");
    expect(TT::Indent, "indented body");

    while (!check(TT::Dedent) && !check(TT::Eof)) {
        if (check(TT::Kw_fun)) {
            int fln = peek().line;
            expect(TT::Kw_fun, "'fun'");
            auto fn = std::make_shared<Function>();
            fn->name = expectIdent("method name").text;
            fn->line = fln;
            if (check(TT::LParen)) {
                adv();
                fn->params = parseParamList();
                expect(TT::RParen, "')'");
            }
            if (match(TT::Colon)) {
                if (!check(TT::Newline) && !check(TT::FatArrow) &&
                    !check(TT::Colon) && tokenStartsType(peek().type)) {
                    fn->retType = parseType();
                }
                if (match(TT::Colon)) {
                    fn->body    = parseSuite();
                    fn->hasBody = true;
                } else if (match(TT::FatArrow)) {
                    auto e = parseExpr();
                    auto r = std::make_unique<RetStmt>();
                    r->line  = e->line;
                    r->value = std::move(e);
                    fn->body.push_back(std::move(r));
                    fn->hasBody = true;
                }
            } else if (match(TT::FatArrow)) {
                auto e = parseExpr();
                auto r = std::make_unique<RetStmt>();
                r->line  = e->line;
                r->value = std::move(e);
                fn->body.push_back(std::move(r));
                fn->hasBody = true;
            }
            tra->methods.push_back(fn);
        } else {
            fail("expected 'fun' inside trait body");
        }
        skipNewlines();
    }
    expect(TT::Dedent, "dedent");

    prog.traits[name] = tra;
    prog.traitsByType[tra->forTypeName].push_back(tra);

    TopLevel tl;
    tl.kind = TopLevel::Tra;
    tl.tra  = tra;
    tl.line = ln;
    return tl;
}

inline TopLevel Parser::parseTypeAlias(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_typ, "'typ'");
    std::string name = expectIdent("type alias name").text;

    auto td = std::make_shared<TypDef>();
    td->name = name;
    td->line = ln;

    if (check(TT::Lt)) td->genericParams = parseGenericParams();
    expect(TT::Assign, "'='");
    td->definition = parseType();

    prog.typedefs[name] = td;

    TopLevel tl;
    tl.kind = TopLevel::Alias;
    tl.typ  = td;
    tl.line = ln;
    return tl;
}

// ============================================================================
// P5-F  类
// ============================================================================
inline void validateCtorOverloads(const ClassDecl& cls) {
    std::unordered_map<size_t, int> ctorParamCounts;
    for (auto& m : cls.members) {
        if (m.kind != ClassMember::Ctor || !m.method) continue;
        size_t pc = m.method->params.size();
        if (ctorParamCounts.count(pc)) {
            throw ArcError::at(cls.line, 1,
                "class '" + cls.name + "' has multiple ini with " +
                std::to_string(pc) + " params; "
                "parameter counts must be distinct");
        }
        ctorParamCounts[pc] = 1;
    }
}

inline TopLevel Parser::parseClassDef(Program& prog, bool isFinal) {
    int ln = peek().line;
    if (isFinal) expect(TT::Kw_fnl, "'fnl'");
    expect(TT::Kw_cls, "'cls'");
    std::string name = expectIdent("class name").text;

    auto cls = std::make_shared<ClassDecl>();
    cls->name    = name;
    cls->isFinal = isFinal;
    cls->line    = ln;

    if (check(TT::Lt)) cls->genericParams = parseGenericParams();
    if (match(TT::Kw_sup)) cls->superClass = expectIdent("super class").text;

    expect(TT::Colon, "':'");
    parseClassBody(*cls);

    validateCtorOverloads(*cls);

    prog.classes[name] = cls;

    TopLevel tl;
    tl.kind = TopLevel::Cls;
    tl.cls  = cls;
    tl.line = ln;
    return tl;
}

// ============================================================================
// 批次 2.5：类继承完整性
// ============================================================================
inline void Parser::validateClasses(Program& prog) {
    using MethodMap = std::unordered_map<std::string, FunctionPtr>;

    std::function<void(const ClassDecl&, MethodMap&)> collectAll =
        [&](const ClassDecl& c, MethodMap& out) {
            if (!c.superClass.empty()) {
                auto it = prog.classes.find(c.superClass);
                if (it != prog.classes.end()) collectAll(*it->second, out);
            }
            for (auto& m : c.members)
                if (m.method) out[m.method->name] = m.method;
        };

    for (auto& kv : prog.classes) {
        const std::string& cname = kv.first;
        ClassDecl& cls = *kv.second;

        if (!cls.superClass.empty()) {
            auto sit = prog.classes.find(cls.superClass);
            if (sit != prog.classes.end() && sit->second->isFinal) {
                throw ArcError::at(cls.line, 1,
                    "class '" + cname + "' cannot extend final class '"
                    + cls.superClass + "'");
            }
        }

        MethodMap parentMethods;
        if (!cls.superClass.empty()) {
            auto sit = prog.classes.find(cls.superClass);
            if (sit != prog.classes.end())
                collectAll(*sit->second, parentMethods);
        }
        for (auto& m : cls.members) {
            if (!m.method) continue;
            if (m.kind == ClassMember::Ctor ||
                m.kind == ClassMember::Dtor) continue;
            auto pit = parentMethods.find(m.method->name);
            if (pit == parentMethods.end()) continue;
            FunctionPtr par = pit->second;
            FunctionPtr sub = m.method;

            if (par->isFinal && par->isVirtual) {
                throw ArcError::at(m.line, 1,
                    "method '" + m.method->name +
                    "' cannot override final virtual method in " +
                    cls.superClass);
            }

            if (par->params.size() != sub->params.size()) {
                throw ArcError::at(m.line, 1,
                    "method '" + m.method->name +
                    "' overrides with different parameter count: parent has " +
                    std::to_string(par->params.size()) + ", subclass has " +
                    std::to_string(sub->params.size()));
            }

            for (size_t pi = 0; pi < par->params.size(); ++pi) {
                if (par->params[pi].name == "self") continue;
                std::string pty = par->params[pi].type
                    ? par->params[pi].type->toString() : std::string("?");
                std::string sty = sub->params[pi].type
                    ? sub->params[pi].type->toString() : std::string("?");
                if (pty != sty) {
                    throw ArcError::at(m.line, 1,
                        "method '" + m.method->name + "' parameter " +
                        std::to_string(pi) + " type mismatch on override: "
                        "parent '" + pty + "', subclass '" + sty + "'");
                }
            }
        }
    }
}

inline void Parser::parseClassBody(ClassDecl& cls) {
    expect(TT::Newline, "newline after class header");
    expect(TT::Indent, "indented class body");

    ClassMember::Access acc = ClassMember::Private;

    while (!check(TT::Dedent) && !check(TT::Eof)) {
        if (check(TT::Kw_pub) || check(TT::Kw_pri) || check(TT::Kw_pro)) {
            ClassMember::Access a = ClassMember::Private;
            if (check(TT::Kw_pub)) a = ClassMember::Public;
            if (check(TT::Kw_pri)) a = ClassMember::Private;
            if (check(TT::Kw_pro)) a = ClassMember::Protected;
            adv();
            parseAccessBlock(cls, a);
        } else {
            parseClassMember(cls, acc);
        }
        skipNewlines();
    }
    expect(TT::Dedent, "dedent of class body");
}

inline void Parser::parseAccessBlock(ClassDecl& cls, ClassMember::Access acc) {
    expect(TT::Colon, "':' after access keyword");
    expect(TT::Newline, "newline after access block header");
    expect(TT::Indent, "indented access block");

    while (!check(TT::Dedent) && !check(TT::Eof)) {
        if (check(TT::Kw_pub) || check(TT::Kw_pri) || check(TT::Kw_pro)) {
            fail("nested access blocks are not allowed");
        }
        parseClassMember(cls, acc);
        skipNewlines();
    }
    expect(TT::Dedent, "dedent of access block");
}

inline void Parser::parseClassMember(ClassDecl& cls, ClassMember::Access acc) {
    int ln = peek().line;

    if (check(TT::Kw_var)) {
        adv();
        do {
            ClassMember m;
            m.kind       = ClassMember::Field;
            m.access     = acc;
            m.line       = ln;
            m.field.name = expectIdent("field name").text;
            expect(TT::Colon, "':'");
            m.field.type = parseType();
            cls.members.push_back(std::move(m));
        } while (match(TT::Comma));
        return;
    }

    bool nestedFinal = false;
    if (check(TT::Kw_cls)) {
    } else if (check(TT::Kw_fnl) && peekAt(1).type == TT::Kw_cls) {
        nestedFinal = true;
    }
    if (check(TT::Kw_cls) || nestedFinal) {
        if (nestedFinal) adv();
        expect(TT::Kw_cls, "'cls'");

        auto nested = std::make_shared<ClassDecl>();
        nested->name    = expectIdent("nested class name").text;
        nested->isFinal = nestedFinal;
        nested->line    = ln;

        if (check(TT::Lt)) nested->genericParams = parseGenericParams();
        if (match(TT::Kw_sup)) nested->superClass = expectIdent("super class").text;
        expect(TT::Colon, "':'");
        parseClassBody(*nested);

        validateCtorOverloads(*nested);

        ClassMember m;
        m.kind        = ClassMember::NestedClass;
        m.access      = acc;
        m.line        = ln;
        m.nestedClass = nested;
        cls.members.push_back(std::move(m));
        return;
    }

    if (check(TT::Kw_frn)) {
        adv();
        ClassMember m;
        m.kind   = ClassMember::FriendFun;
        m.access = acc;
        m.line   = ln;
        if (match(TT::Kw_cls)) {
            m.kind       = ClassMember::FriendCls;
            m.friendName = expectIdent("friend class").text;
        } else {
            match(TT::Kw_fun);
            m.friendName = expectIdent("friend function").text;
        }
        cls.members.push_back(std::move(m));
        return;
    }

    bool mFinal = false, mStatic = false, mVirtual = false, mAbstract = false;
    if (check(TT::Kw_fnl)) { adv(); mFinal = true; }
    if (check(TT::Kw_sta)) { adv(); mStatic = true; }
    if (check(TT::Kw_vir)) { adv(); mVirtual = true; }
    if (check(TT::Kw_abs)) { adv(); mAbstract = true; }

    if (check(TT::Kw_fun)) {
        int fln = peek().line;
        adv();

        ClassMember m;
        m.access = acc;
        m.line   = fln;

        if (check(TT::Kw_ini) || check(TT::Kw_fin)) {
            bool isCtor = check(TT::Kw_ini);
            adv();
            auto fn = std::make_shared<Function>();
            fn->name       = isCtor ? "ini" : "fin";
            fn->ownerClass = cls.name;
            fn->line       = fln;
            if (check(TT::LParen)) {
                adv();
                fn->params = parseParamList();
                expect(TT::RParen, "')'");
            }
            {
                bool selfOk = !fn->params.empty()
                    && fn->params[0].name == "self"
                    && fn->params[0].type
                    && fn->params[0].type->kind == Type::Ptr
                    && !fn->params[0].type->args.empty();
                if (selfOk) {
                    std::string inner = fn->params[0].type->args[0]->name;
                    auto dp = inner.find('$');
                    if (dp != std::string::npos) inner = inner.substr(0, dp);
                    if (inner != cls.name) selfOk = false;
                }
                if (!selfOk) {
                    fail((isCtor ? std::string("ini") : std::string("fin")) +
                         " must declare 'self *" + cls.name +
                         "' as first parameter");
                }
            }
            if (match(TT::Colon)) {
                bool hasRetType = !check(TT::Newline) && !check(TT::Colon) &&
                                  tokenStartsType(peek().type);
                if (hasRetType) fn->retType = parseType();
                if (match(TT::Colon)) {
                    fn->body    = parseSuite();
                    fn->hasBody = true;
                } else if (match(TT::FatArrow)) {
                    auto e = parseExpr();
                    auto r = std::make_unique<RetStmt>();
                    r->line  = e->line;
                    r->value = std::move(e);
                    fn->body.push_back(std::move(r));
                    fn->hasBody = true;
                } else if (!hasRetType && check(TT::Newline)) {
                    fn->body    = parseSuite();
                    fn->hasBody = true;
                }
            } else if (match(TT::FatArrow)) {
                auto e = parseExpr();
                auto r = std::make_unique<RetStmt>();
                r->line  = e->line;
                r->value = std::move(e);
                fn->body.push_back(std::move(r));
                fn->hasBody = true;
            }
            m.kind   = isCtor ? ClassMember::Ctor : ClassMember::Dtor;
            m.method = fn;
            cls.members.push_back(std::move(m));
            return;
        }

        std::string name = expectIdent("method name").text;
        auto fn = std::make_shared<Function>();
        fn->name       = name;
        fn->ownerClass = cls.name;
        fn->line       = fln;
        fn->isStatic   = mStatic;
        fn->isVirtual  = mVirtual;
        fn->isAbstract = mAbstract;
        fn->isFinal    = mFinal;

        if (check(TT::Lt)) fn->genericParams = parseGenericParams();
        if (check(TT::LParen)) {
            adv();
            fn->params = parseParamList();
            expect(TT::RParen, "')'");
        }

        if (match(TT::FatArrow)) {
            if (mAbstract)
                throw ArcError::at(fln, 1,
                    "abstract method '" + name + "' cannot have a body");
            auto e = parseExpr();
            auto r = std::make_unique<RetStmt>();
            r->line  = e->line;
            r->value = std::move(e);
            fn->body.push_back(std::move(r));
            fn->hasBody = true;
        } else if (match(TT::Colon)) {
            bool hasRetType = !check(TT::Newline) && !check(TT::FatArrow) &&
                              !check(TT::Colon) && tokenStartsType(peek().type);
            if (hasRetType) fn->retType = parseType();

            if (mAbstract) {
                if (check(TT::Colon) || check(TT::FatArrow))
                    throw ArcError::at(fln, 1,
                        "abstract method '" + name + "' cannot have a body");
            } else if (match(TT::Colon)) {
                fn->body    = parseSuite();
                fn->hasBody = true;
            } else if (match(TT::FatArrow)) {
                auto e = parseExpr();
                auto r = std::make_unique<RetStmt>();
                r->line  = e->line;
                r->value = std::move(e);
                fn->body.push_back(std::move(r));
                fn->hasBody = true;
            } else if (!hasRetType && check(TT::Newline)) {
                fn->body    = parseSuite();
                fn->hasBody = true;
            }
        }

        if (mAbstract)      m.kind = ClassMember::AbstractMethod;
        else if (mStatic)   m.kind = ClassMember::StaticMethod;
        else if (mVirtual)  m.kind = mFinal ? ClassMember::FinalMethod
                                            : ClassMember::VirtualMethod;
        else if (mFinal)
            throw ArcError::at(fln, 1,
                "'fnl' on non-virtual method '" + name +
                "' requires 'vir'");
        else                m.kind = ClassMember::Method;

        m.method = fn;
        cls.members.push_back(std::move(m));
        return;
    }

    fail("unexpected token inside class body: '" + peek().text + "'");
}

// ============================================================================
// P5-G  模块
// ============================================================================
inline TopLevel Parser::parseModuleDef(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_mod, "'mod'");
    std::string name = expectIdent("module name").text;

    auto mod = std::make_shared<ModuleDecl>();
    mod->name = name;
    mod->line = ln;

    expect(TT::Colon, "':'");
    expect(TT::Newline, "newline after mod header");
    expect(TT::Indent, "indented module body");

    prog.modules[name] = mod;
    moduleStack_.push_back(name);

    while (!check(TT::Dedent) && !check(TT::Eof)) {
        TopLevel inner = parseTopLevel(prog);
        if (inner.kind != TopLevel::Empty)
            prog.topLevel.push_back(std::move(inner));
        skipNewlines();
    }
    moduleStack_.pop_back();
    expect(TT::Dedent, "dedent of module body");

    TopLevel tl;
    tl.kind = TopLevel::Mod;
    tl.mod  = mod;
    tl.line = ln;
    return tl;
}

// ============================================================================
// P5-H  sext 元编程
// ============================================================================
inline TopLevel Parser::parseSextBlock(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_sext, "'sext'");
    expect(TT::Colon, "':'");

    if (check(TT::Ident) && peekAt(1).type == TT::LParen) {
        auto blk = std::make_shared<SextBlock>();
        blk->line = ln;
        SextStmt s;
        s.line = ln;
        s.kind = SextStmt::CteCall;
        s.name = adv().text;
        expect(TT::LParen, "'('");
        s.cteArgs.push_back(parseExpr());
        while (match(TT::Comma)) s.cteArgs.push_back(parseExpr());
        expect(TT::RParen, "')'");
        blk->stmts.push_back(std::move(s));
        execSextBlock(blk.get(), prog);
        TopLevel tl;
        tl.kind = TopLevel::Sext;
        tl.sext = blk;
        tl.line = ln;
        return tl;
    }

    expect(TT::Newline, "newline after sext header");
    expect(TT::Indent, "indented sext body");

    auto blk = std::make_shared<SextBlock>();
    blk->line = ln;
    parseSextSuite(blk->stmts);
    expect(TT::Dedent, "dedent of sext body");

    execSextBlock(blk.get(), prog);

    TopLevel tl;
    tl.kind = TopLevel::Sext;
    tl.sext = blk;
    tl.line = ln;
    return tl;
}

inline void Parser::parseSextSuite(std::vector<SextStmt>& out) {
    while (!check(TT::Dedent) && !check(TT::Eof)) {
        out.push_back(parseSextStmt());
        skipNewlines();
    }
}

inline SextStmt Parser::parseSextStmt() {
    SextStmt s;
    s.line = peek().line;

    if (check(TT::Kw_rep)) {
        adv();
        s.kind = SextStmt::Rep;
        s.expr = parseExpr();
        expect(TT::Colon, "':'");
        expect(TT::Newline, "newline after rep header");
        expect(TT::Indent, "indented rep body");
        parseSextSuite(s.body);
        expect(TT::Dedent, "dedent of rep body");
        return s;
    }

    if (check(TT::Kw_if)) {
        adv();
        s.kind = SextStmt::If;
        s.expr = parseExpr();
        expect(TT::Colon, "':'");
        expect(TT::Newline, "newline after sext if header");
        expect(TT::Indent, "indented if body");
        parseSextSuite(s.body);
        expect(TT::Dedent, "dedent of sext if body");
        if (match(TT::Kw_els)) {
            expect(TT::Colon, "':'");
            expect(TT::Newline, "newline after els header");
            expect(TT::Indent, "indented els body");
            parseSextSuite(s.elseBody);
            expect(TT::Dedent, "dedent of sext els body");
        }
        return s;
    }

    if (check(TT::Kw_for)) {
        adv();
        s.kind = SextStmt::For;
        s.name = expectIdent("loop variable").text;
        if (!checkIdent("in")) fail("expected 'in'");
        adv();
        s.arg1 = parseExpr();
        expect(TT::DotDot, "'..'");
        s.arg2 = parseExpr();
        expect(TT::Colon, "':'");
        expect(TT::Newline, "newline after for header");
        expect(TT::Indent, "indented for body");
        parseSextSuite(s.body);
        expect(TT::Dedent, "dedent of sext for body");
        return s;
    }

    if (check(TT::Kw_set)) {
        adv();
        s.kind = SextStmt::Set;
        s.name = expectIdent("variable name").text;
        expect(TT::Comma, "','");
        s.expr = parseExpr();
        return s;
    }

    if (check(TT::Kw_equ)) {
        adv();
        s.kind = SextStmt::Equ;
        s.name = expectIdent("constant name").text;
        expect(TT::Assign, "'='");
        s.expr = parseExpr();
        return s;
    }

    if (check(TT::Kw_lod)) {
        adv();
        s.kind = SextStmt::Lod;
        if (check(TT::StrLit)) s.text = adv().sval;
        else s.text = expectIdent("package name").text;
        return s;
    }

    if (check(TT::Kw_upd)) {
        adv();
        s.kind = SextStmt::Upd;
        s.name = expectIdent("package name").text;
        return s;
    }

    if (check(TT::Kw_inc)) {
        adv();
        s.kind = SextStmt::Inc;
        if (match(TT::Lt)) {
            s.text = expectIdent("file name").text;
            while (match(TT::Dot)) {
                s.text += ".";
                s.text += expectIdent("file name segment").text;
            }
            expect(TT::Gt, "'>'");
        } else {
            s.text = expect(TT::StrLit, "file name").sval;
        }
        return s;
    }

    if (check(TT::Kw_alc)) {
        adv();
        s.kind = SextStmt::Alc;
        s.name = expectIdent("target variable").text;
        expect(TT::Comma, "','");
        if (checkIdent("code")) {
            adv();
            expect(TT::Colon, "':'");
            s.text = "code";
            s.arg1 = parseExpr();
        } else {
            s.arg1 = parseExpr();
        }
        return s;
    }

    if (check(TT::Kw_gen)) {
        adv();
        s.kind = SextStmt::Gen;
        s.text = expect(TT::StrLit, "string").sval;
        return s;
    }

    if (checkIdent("def_attr")) {
        adv();
        s.kind = SextStmt::DefAttr;
        s.name = expectIdent("attribute name").text;
        expect(TT::Comma, "','");
        s.text = expect(TT::StrLit, "description").sval;
        expect(TT::Comma, "','");
        s.arg1 = parseExpr();
        return s;
    }

    if (checkIdent("def_mac")) {
        adv();
        s.kind = SextStmt::DefMac;
        s.name = expectIdent("macro name").text;
        expect(TT::Comma, "','");
        s.macroPattern = parsePattern();
        expect(TT::Comma, "','");
        s.arg1 = parseExpr();
        s.arg2 = nullptr;
        return s;
    }

    if (check(TT::Kw_cte)) {
        adv();
        expect(TT::Colon, "':'");
        s.kind = SextStmt::CteCall;
        s.name = expectIdent("compile-time function name").text;
        expect(TT::LParen, "'('");
        s.cteArgs.push_back(parseExpr());
        while (match(TT::Comma)) s.cteArgs.push_back(parseExpr());
        expect(TT::RParen, "')'");
        return s;
    }

    fail("unexpected token inside sext body: '" + peek().text + "'");
}

// ============================================================================
// P5-I  use / pkg / lod / upd / exp / ext / equ / var / attr
// ============================================================================
inline TopLevel Parser::parseUseStmt(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_use, "'use'");
    TopLevel tl;
    tl.kind = TopLevel::Use;
    tl.line = ln;

    tl.s1 = expectIdent("module name").text;

    while (match(TT::Dot)) {
        if (check(TT::Star)) {
            adv();
            tl.s2 = "*";
            break;
        }
        tl.s1 += ".";
        tl.s1 += expectIdent("member name").text;
    }

    std::string aliasName;
    if (check(TT::Kw_as)) {
        if (tl.s2 == "*") {
            throw ArcError::at(ln, 1,
                "use: cannot alias a star-import ('mod.* as X' is not allowed)");
        }
        adv();
        aliasName = expectIdent("alias").text;
        tl.s2 = aliasName;
    }

    if (tl.s2 != "*" && tl.s1.find('.') == std::string::npos) {
        throw ArcError::at(ln, 1,
            "use: bare 'use " + tl.s1 +
            "' is not allowed; use 'use " + tl.s1 + ".member' or 'use " +
            tl.s1 + ".*'");
    }

    if (tl.s2 == "*" && tl.s1.find('.') == std::string::npos) {
        auto mit = prog.modules.find(tl.s1);
        if (mit == prog.modules.end() || !mit->second)
            throw ArcError::at(ln, 1, "use: unknown module '" + tl.s1 + "'");
        auto& mod = *mit->second;
        for (auto& full : mod.members) {
            if (!mod.exported.count(full)) continue;
            auto pos = full.find('$');
            std::string shortName = (pos == std::string::npos)
                                    ? full : full.substr(pos + 1);
            prog.useAliases[shortName] = full;
        }
    } else {
        auto pos = tl.s1.find('.');
        if (pos != std::string::npos) {
            std::string modName    = tl.s1.substr(0, pos);
            std::string memberName = tl.s1.substr(pos + 1);
            std::string target     = modName + "$" + memberName;
            auto mit = prog.modules.find(modName);
            if (mit == prog.modules.end() || !mit->second)
                throw ArcError::at(ln, 1,
                    "use: unknown module '" + modName + "'");
            auto& mod = *mit->second;
            bool found = false;
            for (auto& m : mod.members) if (m == target) { found = true; break; }
            if (!found)
                throw ArcError::at(ln, 1,
                    "use: module '" + modName + "' has no member '" +
                    memberName + "'");
            if (!mod.exported.count(target))
                throw ArcError::at(ln, 1,
                    "use: member '" + memberName +
                    "' is not exported from module '" + modName + "'");
            std::string localName  = aliasName.empty() ? memberName : aliasName;
            prog.useAliases[localName] = target;
        }
    }

    return tl;
}

inline TopLevel Parser::parsePkgStmt(Program& prog) {
    (void)prog;
    int ln = peek().line;
    expect(TT::Kw_pkg, "'pkg'");
    TopLevel tl;
    tl.kind = TopLevel::Pkg;
    tl.line = ln;
    tl.s1 = expectIdent("package name").text;
    if (match(TT::Comma)) {
        expect(TT::Ident, "'ver'");
        tl.s2 = expect(TT::StrLit, "version string").sval;
    }
    return tl;
}

inline TopLevel Parser::parseLodStmt(Program& prog) {
    (void)prog;
    int ln = peek().line;
    expect(TT::Kw_lod, "'lod'");
    TopLevel tl;
    tl.kind = TopLevel::Lod;
    tl.line = ln;
    tl.s1 = expectIdent("package name").text;
    if (check(TT::Ident) && peek().text == "from") {
        adv();
        tl.s2 = expect(TT::StrLit, "path/URL").sval;
    }
    return tl;
}

inline TopLevel Parser::parseUpdStmt(Program& prog) {
    (void)prog;
    int ln = peek().line;
    expect(TT::Kw_upd, "'upd'");
    TopLevel tl;
    tl.kind = TopLevel::Upd;
    tl.line = ln;
    tl.s1 = expectIdent("package name").text;
    return tl;
}

inline TopLevel Parser::parseExpDecl(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_exp, "'exp'");

    auto finish = [&](TopLevel tl) -> TopLevel {
        tl.kind = TopLevel::Exp;
        tl.line = ln;
        if (moduleStack_.empty()) return tl;
        std::string modName = moduleStack_.back();
        std::string localName;
        if      (tl.fun)  localName = tl.fun->name;
        else if (tl.rec)  localName = tl.rec->name;
        else if (tl.enm)  localName = tl.enm->name;
        else if (tl.cls)  localName = tl.cls->name;
        else if (tl.sig)  localName = tl.sig->name;
        else if (tl.tra)  localName = tl.tra->name;
        else if (tl.typ)  localName = tl.typ->name;
        else if (tl.stmt) {
            if (auto* vd = dynamic_cast<VarDeclStmt*>(tl.stmt.get());
                vd && !vd->vars.empty()) {
                localName = vd->vars[0].name;
            }
        }
        if (!localName.empty()) {
            auto mit = prog.modules.find(modName);
            if (mit != prog.modules.end() && mit->second) {
                auto& mod = *mit->second;
                std::string fullName = modName + "$" + localName;
                mod.exported.insert(fullName);
                if (!tl.fun) {
                    bool exists = false;
                    for (auto& m : mod.members)
                        if (m == fullName) { exists = true; break; }
                    if (!exists) mod.members.push_back(fullName);
                }
            }
        }
        return tl;
    };

    if (check(TT::Kw_fun)) {
        return finish(parseFunction(prog, false));
    }
    if (check(TT::Kw_var)) {
        TopLevel inner = parseGlobalVarDecl(prog);
        TopLevel tl;
        tl.stmt = std::move(inner.stmt);
        return finish(std::move(tl));
    }
    if (check(TT::Kw_rec)) return finish(parseRecordDef(prog));
    if (check(TT::Kw_enm)) return finish(parseEnumDef(prog));
    if (check(TT::Kw_cls) || check(TT::Kw_fnl)) {
        bool isFinal = check(TT::Kw_fnl);
        return finish(parseClassDef(prog, isFinal));
    }
    if (check(TT::Kw_sig)) return finish(parseSigDef(prog));
    if (check(TT::Kw_tra)) return finish(parseTraitDef(prog));
    if (check(TT::Kw_typ)) return finish(parseTypeAlias(prog));
    if (check(TT::Ident)) {
        TopLevel tl;
        tl.s1 = adv().text;
        return finish(std::move(tl));
    }
    fail("expected declaration after 'exp'");
}

inline TopLevel Parser::parseExtDecl(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_ext, "'ext'");

    if (check(TT::Kw_fun)) {
        TopLevel inner = parseFunction(prog, false);
        TopLevel tl;
        tl.kind = TopLevel::Ext;
        tl.line = ln;
        tl.fun  = inner.fun;
        tl.s1   = inner.fun->name;
        return tl;
    }
    if (check(TT::Kw_var)) {
        TopLevel inner = parseGlobalVarDecl(prog);
        TopLevel tl;
        tl.kind = TopLevel::Ext;
        tl.line = ln;
        tl.stmt = std::move(inner.stmt);
        if (auto* v = dynamic_cast<VarDeclStmt*>(tl.stmt.get());
            v && !v->vars.empty()) {
            tl.s1 = v->vars[0].name;
        }
        return tl;
    }
    if (check(TT::Ident)) {
        TopLevel tl;
        tl.kind = TopLevel::Ext;
        tl.line = ln;
        tl.s1   = adv().text;
        return tl;
    }
    fail("expected declaration after 'ext'");
}

inline TopLevel Parser::parseEquStmt(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_equ, "'equ'");
    TopLevel tl;
    tl.kind = TopLevel::Equ;
    tl.line = ln;
    tl.s1   = expectIdent("constant name").text;
    expect(TT::Assign, "'='");
    auto e = parseExpr();
    auto cv = tryEvalConst(e.get(), prog);
    if (!cv) {
        throw ArcError::at(ln, 1,
            "equ '" + tl.s1 + "': expression is not a compile-time constant");
    }
    consts_[tl.s1] = *cv;
    auto ie = std::make_unique<IntExpr>(*cv);
    ie->line = e->line;
    ie->col  = e->col;
    e = std::move(ie);
    auto st = std::make_unique<ExprStmt>(std::move(e));
    st->line = ln;
    tl.stmt = std::move(st);
    return tl;
}

inline TopLevel Parser::parseGlobalVarDecl(Program& prog) {
    int ln = peek().line;
    expect(TT::Kw_var, "'var'");
    TopLevel tl;
    tl.kind = TopLevel::GlobalVar;
    tl.line = ln;

    std::string owner;
    if (!moduleStack_.empty()) owner = moduleStack_.back();

    auto decl = std::make_unique<VarDeclStmt>();
    decl->line = ln;

    do {
        VarDeclStmt::Item it;
        it.name = expectIdent("variable name").text;
        expect(TT::Colon, "':'");
        it.type = parseType();
        it.ownerModule = owner;
        decl->vars.push_back(std::move(it));
    } while (match(TT::Comma));

    tl.stmt = std::move(decl);
    return tl;
}

inline TopLevel Parser::parseAttrTopLevel(Program& prog) {
    (void)prog;
    int ln = peek().line;
    adv();

    auto s = std::make_unique<AttrStmt>();
    s->line     = ln;
    s->varName  = expectIdent("target name").text;
    expect(TT::Comma, "','");
    s->attrName = expectIdent("attribute name").text;
    if (match(TT::Comma)) s->arg = parseExpr();

    TopLevel tl;
    tl.kind = TopLevel::Attr;
    tl.line = ln;
    tl.stmt = std::move(s);
    return tl;
}

// P5 结束。namespace arc 保持打开。
// ============================================================================
// P6  语法解析器（中）—— 语句、模式
// ============================================================================

inline std::vector<StmtPtr> Parser::parseSuite() {
    expect(TT::Newline, "newline before block");
    expect(TT::Indent,  "indented block");

    std::vector<StmtPtr> body;
    while (!check(TT::Dedent) && !check(TT::Eof)) {
        if (check(TT::Indent)) {
            if (body.empty() ||
                dynamic_cast<LabelStmt*>(body.back().get()) == nullptr) {
                fail("unexpected indent");
            }
            adv();
            while (!check(TT::Dedent) && !check(TT::Eof)) {
                body.push_back(parseStmt());
                skipNewlines();
            }
            expect(TT::Dedent, "dedent of nested block");
            continue;
        }
        body.push_back(parseStmt());
        skipNewlines();
    }
    expect(TT::Dedent, "dedent at end of block");

    for (size_t i = 0; i + 1 < body.size(); ++i) {
        auto* lbl = dynamic_cast<LabelStmt*>(body[i].get());
        if (!lbl) continue;
        Stmt* nxt = body[i + 1].get();
        if (auto* w = dynamic_cast<WhileStmt*>(nxt)) {
            if (w->label.empty()) w->label = lbl->name;
        } else if (auto* fr = dynamic_cast<ForRangeStmt*>(nxt)) {
            if (fr->label.empty()) fr->label = lbl->name;
        } else if (auto* fc = dynamic_cast<ForCStmt*>(nxt)) {
            if (fc->label.empty()) fc->label = lbl->name;
        } else if (auto* sw = dynamic_cast<SwitchStmt*>(nxt)) {
            if (sw->label.empty()) sw->label = lbl->name;
        }
    }

    return body;
}

inline StmtPtr Parser::parseStmt() {
    switch (peek().type) {
        case TT::Kw_var:  return parseVarDeclStmt(false);
        case TT::Kw_reg:  { adv(); return parseVarDeclStmt(true); }
        case TT::Kw_if:   return parseIfStmt();
        case TT::Kw_whl:  return parseWhileStmt();
        case TT::Kw_for:  return parseForStmt();
        case TT::Kw_swt:  return parseSwitchStmt();
        case TT::Kw_ret:  return parseRetStmt();
        case TT::Kw_prn: case TT::Kw_put:
        case TT::Kw_epu: case TT::Kw_epr:  return parsePrintStmt();
        case TT::Kw_get:  return parseGetStmt();
        case TT::Kw_new:  return parseNewStmt();
        case TT::Kw_del:  return parseDelStmt();
        case TT::Kw_asm:  return parseAsmStmt();
        case TT::Kw_unw:  return parseUnwStmt();
        case TT::Kw_inc:  return parseIncDecStmt(true);
        case TT::Kw_dec:  return parseIncDecStmt(false);
        case TT::Kw_mov:  return parseMovStmt();
        case TT::Kw_set:  return parseSetStmt();
        case TT::Kw_swp:  return parseSwpStmt();

        case TT::Kw_brk: {
            int ln = peek().line;
            adv();
            auto s = std::make_unique<BreakStmt>();
            s->line = ln;
            if (check(TT::Ident)) s->label = adv().text;
            return s;
        }
        case TT::Kw_cnt: {
            int ln = peek().line;
            adv();
            auto s = std::make_unique<ContStmt>();
            s->line = ln;
            if (check(TT::Ident)) s->label = adv().text;
            return s;
        }
        case TT::Kw_jmp: {
            int ln = peek().line;
            adv();
            auto s = std::make_unique<JumpStmt>();
            s->line = ln;
            s->label = expectIdent("label name").text;
            return s;
        }

        default: break;
    }

    if (checkIdent("atr")) return parseAtrStmt();
    if (checkIdent("attr")) return parseAttrStmt();

    if (check(TT::Ident) && peekAt(1).type == TT::Colon
        && peek().text != "blk"
        && peek().text != "fmt"
        && peek().text != "rse") {
        int ln = peek().line;
        std::string name = adv().text;
        adv();
        auto s = std::make_unique<LabelStmt>();
        s->name = name;
        s->line = ln;
        return s;
    }

    int ln = peek().line;
    auto e = parseExpr();
    if (match(TT::Assign)) {
        auto rhs = parseExpr();
        auto s = std::make_unique<AssignStmt>(std::move(e), std::move(rhs));
        s->line = ln;
        return s;
    }
    auto s = std::make_unique<ExprStmt>(std::move(e));
    s->line = ln;
    return s;
}

inline StmtPtr Parser::parseVarDeclStmt(bool isReg) {
    int ln = peek().line;
    expect(TT::Kw_var, "'var'");
    auto s = std::make_unique<VarDeclStmt>();
    s->line = ln;

    do {
        VarDeclStmt::Item it;
        it.isReg = isReg;
        it.name  = expectIdent("variable name").text;
        expect(TT::Colon, "':'");
        it.type = parseType();
        s->vars.push_back(std::move(it));
    } while (match(TT::Comma));

    return s;
}

inline StmtPtr Parser::parseIfStmt() {
    int ln = peek().line;
    expect(TT::Kw_if, "'if'");
    auto s = std::make_unique<IfStmt>();
    s->line = ln;
    s->cond = parseExpr();
    expect(TT::Colon, "':'");
    s->thenBody = parseSuite();

    if (check(TT::Kw_els)) {
        adv();
        if (check(TT::Kw_if)) {
            auto inner = parseIfStmt();
            s->elseBody.push_back(std::move(inner));
        } else {
            expect(TT::Colon, "':'");
            s->elseBody = parseSuite();
        }
        s->hasElse = true;
    }
    return s;
}

inline StmtPtr Parser::parseWhileStmt() {
    int ln = peek().line;
    expect(TT::Kw_whl, "'whl'");
    auto s = std::make_unique<WhileStmt>();
    s->line = ln;
    s->cond = parseExpr();
    expect(TT::Colon, "':'");
    s->body = parseSuite();
    return s;
}

inline StmtPtr Parser::parseForStmt() {
    int ln = peek().line;
    expect(TT::Kw_for, "'for'");

    if (check(TT::Ident) &&
        peekAt(1).type == TT::Ident && peekAt(1).text == "in") {
        auto s = std::make_unique<ForRangeStmt>();
        s->line = ln;
        s->varName = adv().text;
        adv();

        if (check(TT::Kw_rng)) {
            auto rl = parseRangeLit();
            auto* r = dynamic_cast<RangeLitExpr*>(rl.get());
            if (!r) fail("for: bad rng expression");
            s->from      = std::move(r->from);
            s->to        = std::move(r->to);
            s->exclusive = r->exclusive;
            if (r->step) s->step = std::move(r->step);
        } else {
            s->from = parseExpr();
            if (match(TT::DotDotLt)) {
                s->exclusive = true;
                s->to = parseExpr();
            } else if (match(TT::DotDot)) {
                s->exclusive = false;
                s->to = parseExpr();
            } else {
                s->isForEach = true;
            }
        }

        if (!s->isForEach && checkIdent("step") && !s->step) {
            adv();
            s->step = parseExpr();
        }

        expect(TT::Colon, "':'");
        s->body = parseSuite();
        return s;
    }

    auto s = std::make_unique<ForCStmt>();
    s->line = ln;

    if (!check(TT::Semicolon)) {
        auto e = parseExpr();
        if (match(TT::Assign)) {
            auto rhs = parseExpr();
            auto as = std::make_unique<AssignStmt>(std::move(e), std::move(rhs));
            as->line = ln;
            s->init = std::move(as);
        } else {
            auto es = std::make_unique<ExprStmt>(std::move(e));
            es->line = ln;
            s->init = std::move(es);
        }
    }
    expect(TT::Semicolon, "';' after for-init");

    if (!check(TT::Semicolon)) s->cond = parseExpr();
    expect(TT::Semicolon, "';' after for-cond");

    if (!check(TT::Colon)) {
        if (check(TT::Kw_inc) || check(TT::Kw_dec)) {
            bool isInc = check(TT::Kw_inc);
            adv();
            auto tgt = parseExpr();
            auto id = std::make_unique<IncDecStmt>(std::move(tgt), isInc);
            id->line = ln;
            s->incr = std::move(id);
        } else {
            auto e = parseExpr();
            if (match(TT::Assign)) {
                auto rhs = parseExpr();
                auto as = std::make_unique<AssignStmt>(std::move(e), std::move(rhs));
                as->line = ln;
                s->incr = std::move(as);
            } else {
                auto es = std::make_unique<ExprStmt>(std::move(e));
                es->line = ln;
                s->incr = std::move(es);
            }
        }
    }
    expect(TT::Colon, "':' after for-header");
    s->body = parseSuite();
    return s;
}

inline StmtPtr Parser::parseSwitchStmt() {
    int ln = peek().line;
    expect(TT::Kw_swt, "'swt'");
    auto s = std::make_unique<SwitchStmt>();
    s->line    = ln;
    s->subject = parseExpr();
    expect(TT::Colon,   "':'");
    expect(TT::Newline, "newline after swt header");
    expect(TT::Indent,  "indented swt body");

    while (!check(TT::Dedent) && !check(TT::Eof)) {
        if (check(TT::Kw_cas)) {
            adv();
            CaseClause cc;
            cc.line = peek().line;
            cc.pattern = parsePattern();
            if (check(TT::Kw_if)) {
                adv();
                cc.guard = parseExpr();
            }
            expect(TT::Colon, "':'");
            cc.body = parseSuite();
            s->cases.push_back(std::move(cc));
        } else if (check(TT::Kw_def)) {
            adv();
            expect(TT::Colon, "':'");
            s->defBody    = parseSuite();
            s->hasDefault = true;
        } else {
            fail("expected 'cas' or 'def' inside swt body");
        }
        skipNewlines();
    }
    expect(TT::Dedent, "dedent at end of swt body");
    return s;
}

inline StmtPtr Parser::parseRetStmt() {
    int ln = peek().line;
    expect(TT::Kw_ret, "'ret'");
    auto s = std::make_unique<RetStmt>();
    s->line = ln;
    if (!check(TT::Newline) && !check(TT::Dedent) && !check(TT::Eof))
        s->value = parseExpr();
    return s;
}

inline StmtPtr Parser::parsePrintStmt() {
    TT kw = peek().type;
    int ln = peek().line;
    auto s = std::make_unique<PrintStmt>();
    s->line = ln;
    adv();
    s->newline = (kw == TT::Kw_prn || kw == TT::Kw_epr);
    s->stream  = (kw == TT::Kw_epu || kw == TT::Kw_epr)
                 ? PrintStmt::Stderr : PrintStmt::Stdout;
    s->exprs.push_back(parseExpr());
    while (match(TT::Comma)) s->exprs.push_back(parseExpr());
    return s;
}

inline StmtPtr Parser::parseGetStmt() {
    int ln = peek().line;
    expect(TT::Kw_get, "'get'");
    auto s = std::make_unique<GetStmt>();
    s->line = ln;
    s->targets.push_back(parseExpr());
    while (match(TT::Comma)) s->targets.push_back(parseExpr());
    return s;
}

inline StmtPtr Parser::parseNewStmt() {
    int ln = peek().line;
    expect(TT::Kw_new, "'new'");
    auto s = std::make_unique<NewStmt>();
    s->line    = ln;
    s->varName = expectIdent("variable name").text;
    return s;
}

inline StmtPtr Parser::parseDelStmt() {
    int ln = peek().line;
    expect(TT::Kw_del, "'del'");
    auto s = std::make_unique<DelStmt>();
    s->line    = ln;
    s->varName = expectIdent("variable name").text;
    return s;
}

inline StmtPtr Parser::parseAsmStmt() {
    int ln = peek().line;
    expect(TT::Kw_asm, "'asm'");
    auto s = std::make_unique<AsmStmt>();
    s->line = ln;

    if (!check(TT::AsmRaw))
        fail("expected '{ ... }' after 'asm'");
    s->text = adv().text;
    return s;
}

inline StmtPtr Parser::parseUnwStmt() {
    int ln = peek().line;
    expect(TT::Kw_unw, "'unw'");
    auto s = std::make_unique<UnwStmt>();
    s->line    = ln;
    s->varName = expectIdent("variable name").text;
    expect(TT::Comma, "','");
    s->expr = parseExpr();
    expect(TT::Comma, "','");
    s->fallback = parseExpr();
    return s;
}

inline StmtPtr Parser::parseAtrStmt() {
    int ln = peek().line;
    adv();
    auto s = std::make_unique<AtrStmt>(parseExpr());
    s->line = ln;
    return s;
}

inline StmtPtr Parser::parseIncDecStmt(bool isInc) {
    int ln = peek().line;
    adv();
    auto t = parseExpr();
    auto s = std::make_unique<IncDecStmt>(std::move(t), isInc);
    s->line = ln;
    return s;
}

inline StmtPtr Parser::parseMovStmt() {
    int ln = peek().line;
    expect(TT::Kw_mov, "'mov'");
    auto s = std::make_unique<MovAssignStmt>(nullptr, nullptr);
    s->line   = ln;
    s->target = parseExpr();
    expect(TT::Comma, "','");
    s->value  = parseExpr();
    return s;
}

inline StmtPtr Parser::parseSetStmt() {
    int ln = peek().line;
    expect(TT::Kw_set, "'set'");
    auto s = std::make_unique<SetAssignStmt>(nullptr, nullptr);
    s->line   = ln;
    s->target = parseExpr();
    expect(TT::Comma, "','");
    s->value  = parseExpr();
    return s;
}

inline StmtPtr Parser::parseSwpStmt() {
    int ln = peek().line;
    expect(TT::Kw_swp, "'swp'");
    auto s = std::make_unique<SwpStmt>(nullptr, nullptr);
    s->line = ln;
    s->lhs  = parseExpr();
    expect(TT::Comma, "','");
    s->rhs  = parseExpr();
    return s;
}

inline StmtPtr Parser::parseAttrStmt() {
    int ln = peek().line;
    adv();
    auto s = std::make_unique<AttrStmt>();
    s->line    = ln;
    s->varName = expectIdent("variable name").text;
    expect(TT::Comma, "','");

    switch (peek().type) {
        case TT::Kw_con: case TT::Kw_vol: case TT::Kw_aln:
        case TT::Kw_ntr: case TT::Kw_usr: case TT::Kw_reg:
        case TT::Kw_ixl:
            s->attrName = adv().text;
            break;
        case TT::Ident:
            s->attrName = adv().text;
            break;
        default:
            fail("expected attribute name");
    }

    if (match(TT::Comma)) s->arg = parseExpr();
    return s;
}

inline PatternPtr Parser::parsePattern() {
    if (check(TT::Ident) && peekAt(1).type == TT::At) {
        auto p = std::make_shared<Pattern>();
        p->kind = Pattern::Alias;
        p->line = peek().line;
        p->name = adv().text;
        adv();
        p->aliasInner = parsePattern();
        return p;
    }

    auto p = std::make_shared<Pattern>();
    p->line = peek().line;

    if (check(TT::Ident) && peek().text == "_") {
        adv();
        p->kind = Pattern::Wild;
        return p;
    }
    if (check(TT::Kw_var)) {
        adv();
        p->kind = Pattern::Bind;
        p->name = expectIdent("binding name").text;
        return p;
    }
    if (check(TT::Kw_tru)) {
        adv();
        p->kind = Pattern::LitBol;
        p->bval = true;
        return p;
    }
    if (check(TT::Kw_fal)) {
        adv();
        p->kind = Pattern::LitBol;
        p->bval = false;
        return p;
    }
    if (check(TT::Kw_nil)) {
        adv();
        p->kind = Pattern::LitNil;
        return p;
    }
    if (check(TT::IntLit)) {
        p->kind = Pattern::LitInt;
        p->ival = adv().ival;
        return p;
    }
    if (check(TT::FltLit)) {
        p->kind = Pattern::LitFlt;
        p->fval = adv().fval;
        return p;
    }
    if (check(TT::ChrLit)) {
        p->kind = Pattern::LitChr;
        p->cval = adv().cval;
        return p;
    }
    if (check(TT::StrLit)) {
        p->kind = Pattern::LitStr;
        p->sval = adv().sval;
        return p;
    }
    if (check(TT::Kw_enm)) {
        adv();
        p->kind     = Pattern::EnmPattern;
        p->typeName = expectIdent("enum type").text;
        expect(TT::Dot, "'.'");
        p->variant  = expectIdent("variant name").text;
        if (match(TT::LParen)) {
            if (!check(TT::RParen)) {
                p->args.push_back(parsePattern());
                while (match(TT::Comma)) p->args.push_back(parsePattern());
            }
            expect(TT::RParen, "')'");
        }
        return p;
    }
    if (check(TT::Kw_rec)) {
        adv();
        p->kind     = Pattern::RecPattern;
        p->typeName = expectIdent("record type").text;
        expect(TT::LBrace, "'{'");
        if (!check(TT::RBrace)) {
            do {
                expect(TT::Dot, "'.'");
                Pattern::FieldPat fp;
                fp.name = expectIdent("field name").text;
                if (match(TT::Colon)) fp.pattern = parsePattern();
                p->fields.push_back(std::move(fp));
            } while (match(TT::Comma));
        }
        expect(TT::RBrace, "'}'");
        return p;
    }
    if (check(TT::Kw_tup)) {
        adv();
        p->kind = Pattern::TupPattern;
        expect(TT::LParen, "'('");
        if (!check(TT::RParen)) {
            p->args.push_back(parsePattern());
            while (match(TT::Comma)) p->args.push_back(parsePattern());
        }
        expect(TT::RParen, "')'");
        return p;
    }

    fail("expected a pattern");
}

// P6 结束。namespace arc 保持打开。
// ============================================================================
// P7  语法解析器（下）—— 表达式
// ============================================================================

inline ExprPtr Parser::parseExpr() {
    return parsePipeExpr();
}

inline ExprPtr Parser::parsePipeExpr() {
    auto l = parseOrExpr();
    while (check(TT::PipeGt)) {
        int ln = peek().line;
        adv();
        std::string fnName = expectIdent("pipeline target").text;
        std::vector<ExprPtr> args;
        if (match(TT::LParen)) {
            if (!check(TT::RParen)) {
                args.push_back(parseExpr());
                while (match(TT::Comma)) args.push_back(parseExpr());
            }
            expect(TT::RParen, "')'");
        }
        auto e = std::make_unique<PipeExpr>(std::move(l), std::move(fnName));
        e->line = ln;
        e->args = std::move(args);
        l = std::move(e);
    }
    return l;
}

inline ExprPtr Parser::parseOrExpr() {
    auto l = parseAndExpr();
    while (true) {
        if (check(TT::OrOr)) {
            int ln = peek().line;
            adv();
            auto r = parseAndExpr();
            auto e = std::make_unique<BinaryExpr>("||", std::move(l), std::move(r));
            e->line = ln;
            l = std::move(e);
        } else if (check(TT::Kw_or)) {
            int ln = peek().line;
            adv();
            auto r = parseAndExpr();
            auto e = std::make_unique<OrExpr>(std::move(l), std::move(r));
            e->line = ln;
            l = std::move(e);
        } else {
            break;
        }
    }
    return l;
}

inline ExprPtr Parser::parseAndExpr() {
    auto l = parseEqualityExpr();
    while (check(TT::AndAnd)) {
        int ln = peek().line;
        adv();
        auto r = parseEqualityExpr();
        auto e = std::make_unique<BinaryExpr>("&&", std::move(l), std::move(r));
        e->line = ln;
        l = std::move(e);
    }
    return l;
}

inline ExprPtr Parser::parseEqualityExpr() {
    auto l = parseRelationalExpr();
    while (check(TT::EqEq) || check(TT::Neq)) {
        int ln = peek().line;
        std::string op = peek().text;
        adv();
        auto r = parseRelationalExpr();
        auto e = std::make_unique<BinaryExpr>(op, std::move(l), std::move(r));
        e->line = ln;
        l = std::move(e);
    }
    return l;
}

inline ExprPtr Parser::parseRelationalExpr() {
    auto l = parseAdditiveExpr();
    while (check(TT::Lt) || check(TT::Gt) ||
           check(TT::Leq) || check(TT::Geq)) {
        int ln = peek().line;
        std::string op = peek().text;
        adv();
        auto r = parseAdditiveExpr();
        auto e = std::make_unique<BinaryExpr>(op, std::move(l), std::move(r));
        e->line = ln;
        l = std::move(e);
    }
    return l;
}

inline ExprPtr Parser::parseAdditiveExpr() {
    auto l = parseMultiplicativeExpr();
    while (check(TT::Plus) || check(TT::Minus)) {
        int ln = peek().line;
        std::string op = peek().text;
        adv();
        auto r = parseMultiplicativeExpr();
        auto e = std::make_unique<BinaryExpr>(op, std::move(l), std::move(r));
        e->line = ln;
        l = std::move(e);
    }
    return l;
}

inline ExprPtr Parser::parseMultiplicativeExpr() {
    auto l = parsePowerExpr();
    while (check(TT::Star) || check(TT::Slash) || check(TT::Percent)) {
        int ln = peek().line;
        std::string op = peek().text;
        adv();
        auto r = parsePowerExpr();
        auto e = std::make_unique<BinaryExpr>(op, std::move(l), std::move(r));
        e->line = ln;
        l = std::move(e);
    }
    return l;
}

inline ExprPtr Parser::parsePowerExpr() {
    auto l = parseUnaryExpr();
    if (check(TT::Caret)) {
        int ln = peek().line;
        adv();
        auto r = parsePowerExpr();
        auto e = std::make_unique<BinaryExpr>("^", std::move(l), std::move(r));
        e->line = ln;
        return e;
    }
    return l;
}

inline ExprPtr Parser::parseUnaryExpr() {
    const Token& t = peek();

    if (t.type == TT::Minus || t.type == TT::Bang || t.type == TT::Tilde) {
        int ln = t.line, cl = t.col;
        std::string op = t.text;
        adv();
        auto operand = parseUnaryExpr();
        auto e = std::make_unique<UnaryExpr>(op, std::move(operand));
        e->line = ln;
        e->col  = cl;
        return e;
    }
    if (t.type == TT::Amp) {
        int ln = t.line, cl = t.col;
        adv();
        auto operand = parseUnaryExpr();
        auto e = std::make_unique<AddrExpr>(std::move(operand));
        e->line = ln;
        e->col  = cl;
        return e;
    }
    if (t.type == TT::Star) {
        int ln = t.line, cl = t.col;
        adv();
        auto operand = parseUnaryExpr();
        auto e = std::make_unique<DerefExpr>(std::move(operand), false);
        e->line = ln;
        e->col  = cl;
        return e;
    }
    if (t.type == TT::Caret) {
        int ln = t.line, cl = t.col;
        adv();
        auto operand = parseUnaryExpr();
        auto e = std::make_unique<DerefExpr>(std::move(operand), true);
        e->line = ln;
        e->col  = cl;
        return e;
    }
    if (t.type == TT::Hash) {
        int ln = t.line, cl = t.col;
        adv();
        auto operand = parseUnaryExpr();
        auto e = std::make_unique<UnaryExpr>("#", std::move(operand));
        e->line = ln;
        e->col  = cl;
        return e;
    }
    if (t.type == TT::Dollar) {
        int ln = t.line, cl = t.col;
        adv();
        auto ty = parseType();
        auto e = std::make_unique<SizeofExpr>(std::move(ty), false);
        e->line = ln;
        e->col  = cl;
        return e;
    }
    if (t.type == TT::DollarAln) {
        int ln = t.line, cl = t.col;
        adv();
        auto ty = parseType();
        auto e = std::make_unique<SizeofExpr>(std::move(ty), true);
        e->line = ln;
        e->col  = cl;
        return e;
    }
    if (t.type == TT::Kw_as) {
        int ln = t.line, cl = t.col;
        adv();
        auto val = parseUnaryExpr();
        expect(TT::Comma, "','");
        auto ty = parseType();
        auto e = std::make_unique<AsConvExpr>(std::move(val), std::move(ty));
        e->line = ln;
        e->col  = cl;
        return e;
    }

    return parsePostfixExpr();
}

inline ExprPtr Parser::parsePostfixExpr() {
    auto e = parsePrimaryExpr();

    while (true) {
        const Token& t = peek();

        if (t.type == TT::LBracket) {
            int ln = t.line;
            adv();
            auto first = parseExpr();
            if (check(TT::DotDot) || check(TT::DotDotLt)) {
                bool exclusive = check(TT::DotDotLt);
                adv();
                auto to = parseExpr();
                expect(TT::RBracket, "']'");
                auto s = std::make_unique<SliceExpr>(std::move(e),
                                                     std::move(first),
                                                     std::move(to),
                                                     exclusive);
                s->line = ln;
                e = std::move(s);
            } else {
                expect(TT::RBracket, "']'");
                auto i = std::make_unique<IndexExpr>(std::move(e), std::move(first));
                i->line = ln;
                e = std::move(i);
            }
            continue;
        }

        if (t.type == TT::Dot) {
            int ln = t.line;
            adv();

            if (checkIdent("has") && peekAt(1).type != TT::LParen) {
                adv();
                auto o = std::make_unique<OptHasExpr>(std::move(e));
                o->line = ln;
                e = std::move(o);
                continue;
            }
            if (checkIdent("val") && peekAt(1).type != TT::LParen) {
                adv();
                auto o = std::make_unique<OptValExpr>(std::move(e));
                o->line = ln;
                e = std::move(o);
                continue;
            }
            if (check(TT::IntLit)) {
                int idx = static_cast<int>(adv().ival);
                auto ti = std::make_unique<TupleIndexExpr>(std::move(e), idx);
                ti->line = ln;
                e = std::move(ti);
                continue;
            }

            std::string name = expectIdent("member name").text;
            if (check(TT::LParen)) {
                adv();
                std::vector<ExprPtr> args;
                if (!check(TT::RParen)) {
                    args.push_back(parseExpr());
                    while (match(TT::Comma)) args.push_back(parseExpr());
                }
                expect(TT::RParen, "')'");
                auto mc = std::make_unique<MethodCallExpr>(std::move(e),
                                                            std::move(name),
                                                            std::move(args));
                mc->line = ln;
                e = std::move(mc);
            } else {
                auto m = std::make_unique<MemberExpr>(std::move(e), std::move(name));
                m->line = ln;
                e = std::move(m);
            }
            continue;
        }

        if (t.type == TT::LParen) {
            int ln = t.line;
            adv();
            std::vector<ExprPtr> args;
            if (!check(TT::RParen)) {
                args.push_back(parseExpr());
                while (match(TT::Comma)) args.push_back(parseExpr());
            }
            expect(TT::RParen, "')'");
            auto c = std::make_unique<CallExpr>(std::move(e), std::move(args));
            c->line = ln;
            e = std::move(c);
            continue;
        }

        break;
    }
    return e;
}

inline ExprPtr Parser::parsePrimaryExpr() {
    const Token& t = peek();

    switch (t.type) {
        case TT::IntLit: {
            adv();
            auto e = std::make_unique<IntExpr>(t.ival);
            e->line = t.line;
            e->col  = t.col;
            return e;
        }
        case TT::FltLit: {
            adv();
            auto e = std::make_unique<FltExpr>(t.fval);
            e->line = t.line;
            e->col  = t.col;
            return e;
        }
        case TT::ChrLit: {
            adv();
            auto e = std::make_unique<ChrExpr>(t.cval);
            e->line = t.line;
            e->col  = t.col;
            return e;
        }
        case TT::StrLit: {
            adv();
            auto e = std::make_unique<StrExpr>(t.sval);
            e->line = t.line;
            e->col  = t.col;
            return e;
        }
        case TT::Kw_tru: {
            adv();
            auto e = std::make_unique<BolExpr>(true);
            e->line = t.line;
            e->col  = t.col;
            return e;
        }
        case TT::Kw_fal: {
            adv();
            auto e = std::make_unique<BolExpr>(false);
            e->line = t.line;
            e->col  = t.col;
            return e;
        }
        case TT::Kw_nil: {
            adv();
            auto e = std::make_unique<NilExpr>();
            e->line = t.line;
            e->col  = t.col;
            return e;
        }

        case TT::Ident: {
            if (t.text == "fmt") return parseFmtExpr();
            if (t.text == "blk") return parseBlkExpr();
            if (t.text == "rse") {
                int ln = t.line, cl = t.col;
                adv();
                auto operand = parseUnaryExpr();
                auto e = std::make_unique<RseExpr>(std::move(operand));
                e->line = ln;
                e->col  = cl;
                return e;
            }
            adv();
            auto e = std::make_unique<IdentExpr>(t.text);
            e->line = t.line;
            e->col  = t.col;
            return e;
        }

        case TT::LParen: {
            adv();
            auto e = parseExpr();
            expect(TT::RParen, "')'");
            return e;
        }

        case TT::LBracket:      return parseArrayLit();
        case TT::Kw_tup:        return parseTupleLit();
        case TT::Kw_rec:        return parseRecLit();
        case TT::Kw_cls:        return parseClsLit();
        case TT::Kw_enm:        return parseEnmLit();
        case TT::Kw_rng:        return parseRangeLit();
        case TT::Kw_lam:        return parseLambda();
        case TT::Kw_sel:        return parseSelExpr();

        case TT::Kw_ok: {
            adv();
            auto e = std::make_unique<OkExpr>(parseExpr());
            e->line = t.line;
            e->col  = t.col;
            return e;
        }
        case TT::Kw_er: {
            adv();
            auto e = std::make_unique<ErExpr>(parseExpr());
            e->line = t.line;
            e->col  = t.col;
            return e;
        }
        case TT::Kw_try: {
            adv();
            auto e = std::make_unique<TryExpr>(parseExpr());
            e->line = t.line;
            e->col  = t.col;
            return e;
        }
        case TT::Kw_cal: {
            adv();
            auto target = parseExpr();
            std::vector<ExprPtr> args;
            while (match(TT::Comma)) args.push_back(parseExpr());
            auto e = std::make_unique<CalCallExpr>(std::move(target), std::move(args));
            e->line = t.line;
            e->col  = t.col;
            return e;
        }

        case TT::Kw_add: case TT::Kw_sub: case TT::Kw_mul: case TT::Kw_div:
        case TT::Kw_pow: case TT::Kw_neg:
        case TT::Kw_min: case TT::Kw_max:
        case TT::Kw_shl: case TT::Kw_shr: case TT::Kw_lsr:
        case TT::Kw_and: case TT::Kw_xor: case TT::Kw_not:
        case TT::Kw_neq: case TT::Kw_grt: case TT::Kw_lss:
        case TT::Kw_geq: case TT::Kw_leq: {
            int ln = t.line, cl = t.col;
            std::string name = t.text;
            adv();
            return parseNamedInstr(name, ln, cl);
        }

        case TT::Kw_mod: {
            int ln = t.line, cl = t.col;
            adv();
            return parseNamedInstr("mod", ln, cl);
        }
        case TT::Kw_abs: {
            int ln = t.line, cl = t.col;
            adv();
            return parseNamedInstr("abs", ln, cl);
        }
        case TT::Kw_equ: {
            int ln = t.line, cl = t.col;
            adv();
            return parseNamedInstr("equ", ln, cl);
        }
        case TT::Kw_or: {
            int ln = t.line, cl = t.col;
            adv();
            return parseNamedInstr("or", ln, cl);
        }

        case TT::Kw_cte: {
            if (peekAt(1).type != TT::Colon)
                fail("'cte' in expression requires ':' (cte: fn(args))");
            int ln = t.line, cl = t.col;
            adv();
            adv();
            std::string fnName = expectIdent("compile-time function name").text;
            expect(TT::LParen, "'('");
            auto e = std::make_unique<CteCallExpr>(fnName);
            e->line = ln;
            e->col  = cl;
            if (!check(TT::RParen)) {
                e->args.push_back(parseExpr());
                while (match(TT::Comma)) e->args.push_back(parseExpr());
            }
            expect(TT::RParen, "')'");
            return e;
        }

        default: break;
    }

    fail("unexpected token in expression: '" + t.text + "'");
}

inline ExprPtr Parser::parseArrayLit() {
    int ln = peek().line;
    expect(TT::LBracket, "'['");
    auto e = std::make_unique<ArrayLitExpr>();
    e->line = ln;

    if (check(TT::RBracket)) {
        adv();
        return e;
    }

    e->elems.push_back(parseExpr());
    while (match(TT::Comma)) {
        if (check(TT::RBracket)) break;
        e->elems.push_back(parseExpr());
    }
    expect(TT::RBracket, "']'");
    return e;
}

inline ExprPtr Parser::parseTupleLit() {
    int ln = peek().line;
    expect(TT::Kw_tup, "'tup'");
    expect(TT::LParen, "'('");
    auto e = std::make_unique<TupleLitExpr>();
    e->line = ln;
    if (!check(TT::RParen)) {
        e->elems.push_back(parseExpr());
        while (match(TT::Comma)) e->elems.push_back(parseExpr());
    }
    expect(TT::RParen, "')'");
    return e;
}

inline ExprPtr Parser::parseRecLit() {
    int ln = peek().line;
    expect(TT::Kw_rec, "'rec'");
    std::string name = expectIdent("record type").text;
    auto e = std::make_unique<RecLitExpr>(name);
    e->line = ln;

    while (match(TT::Comma)) {
        if (check(TT::Dot)) {
            adv();
            FieldInit fi;
            fi.line  = peek().line;
            fi.name  = expectIdent("field name").text;
            expect(TT::Assign, "'='");
            fi.value = parseExpr();
            e->fields.push_back(std::move(fi));
        } else {
            e->positionalArgs.push_back(parseExpr());
        }
    }
    return e;
}

inline ExprPtr Parser::parseClsLit() {
    int ln = peek().line;
    expect(TT::Kw_cls, "'cls'");
    std::string name = expectIdent("class type").text;
    if (check(TT::Lt)) {
        adv();
        std::vector<TypePtr> args;
        args.push_back(parseType());
        while (match(TT::Comma)) args.push_back(parseType());
        expect(TT::Gt, "'>'");
        std::string mangled = name;
        for (auto& a : args) mangled += "$" + a->toString();
        instantiateIfGeneric(mangled);
        name = mangled;
    }
    auto e = std::make_unique<ClsLitExpr>(name);
    e->line = ln;

    while (match(TT::Comma)) {
        if (check(TT::Dot)) {
            adv();
            FieldInit fi;
            fi.line  = peek().line;
            fi.name  = expectIdent("field name").text;
            expect(TT::Assign, "'='");
            fi.value = parseExpr();
            e->fields.push_back(std::move(fi));
        } else {
            e->positionalArgs.push_back(parseExpr());
        }
    }
    return e;
}

inline ExprPtr Parser::parseEnmLit() {
    int ln = peek().line;
    expect(TT::Kw_enm, "'enm'");
    std::string typeName = expectIdent("enum type").text;
    expect(TT::Dot, "'.'");
    std::string variant = expectIdent("variant name").text;
    auto e = std::make_unique<EnmLitExpr>(typeName, variant);
    e->line = ln;

    if (match(TT::LParen)) {
        if (!check(TT::RParen)) {
            e->args.push_back(parseExpr());
            while (match(TT::Comma)) e->args.push_back(parseExpr());
        }
        expect(TT::RParen, "')'");
    } else if (match(TT::Comma)) {
        e->args.push_back(parseExpr());
        while (match(TT::Comma)) e->args.push_back(parseExpr());
    }
    return e;
}

inline ExprPtr Parser::parseRangeLit() {
    int ln = peek().line;
    expect(TT::Kw_rng, "'rng'");
    auto from = parseExpr();
    bool exclusive = false;
    if (match(TT::DotDotLt))      exclusive = true;
    else if (match(TT::DotDot))   exclusive = false;
    else fail("expected '..' or '..<' in rng literal");
    auto to = parseExpr();

    auto e = std::make_unique<RangeLitExpr>(std::move(from),
                                             std::move(to),
                                             exclusive);
    e->line = ln;

    if (checkIdent("step")) {
        adv();
        e->step = parseExpr();
    }
    return e;
}

inline ExprPtr Parser::parseLambda() {
    int ln = peek().line;
    expect(TT::Kw_lam, "'lam'");
    auto e = std::make_unique<LamExpr>();
    e->line = ln;

    if (match(TT::LBracket)) {
        if (!check(TT::RBracket)) {
            do {
                LambdaCapture cap;
                std::string n1 = expectIdent("capture name").text;
                if (match(TT::Assign)) {
                    cap.outerName = n1;
                    cap.innerName = expectIdent("inner name").text;
                } else {
                    cap.innerName = n1;
                    cap.outerName = n1;
                }
                e->captures.push_back(std::move(cap));
            } while (match(TT::Comma));
        }
        expect(TT::RBracket, "']'");
    }

    expect(TT::LParen, "'('");
    if (!check(TT::RParen)) {
        do {
            std::string pname = expectIdent("param name").text;
            expect(TT::Colon, "':'");
            auto ptype = parseType();
            e->params.emplace_back(std::move(pname), std::move(ptype));
        } while (match(TT::Comma));
    }
    expect(TT::RParen, "')'");

    if (match(TT::FatArrow)) {
        e->single = parseExpr();
    } else if (match(TT::Colon)) {
        if (!check(TT::Newline) && !check(TT::FatArrow) &&
            !check(TT::Colon) && tokenStartsType(peek().type)) {
            e->retType = parseType();
        }
        if (match(TT::Colon)) {
            e->body = parseSuite();
        } else if (match(TT::FatArrow)) {
            e->single = parseExpr();
        } else if (check(TT::Newline)) {
            e->body = parseSuite();
        } else {
            fail("expected ':' or '=>' in lambda body");
        }
    } else {
        fail("expected ':' or '=>' in lambda");
    }
    return e;
}

inline ExprPtr Parser::parseFmtExpr() {
    int ln = peek().line;
    adv();
    auto t = expect(TT::StrLit, "format string");
    auto e = std::make_unique<FmtExpr>();
    e->line = ln;
    const std::string raw = t.sval;

    std::string buf;
    size_t i = 0;
    while (i < raw.size()) {
        char c = raw[i];

        if (c == '{') {
            if (i + 1 < raw.size() && raw[i + 1] == '{') {
                buf += '{';
                i += 2;
                continue;
            }
            size_t j = i + 1;
            int depth = 1;
            while (j < raw.size() && depth > 0) {
                if (raw[j] == '{') ++depth;
                else if (raw[j] == '}') {
                    --depth;
                    if (depth == 0) break;
                }
                ++j;
            }
            if (depth > 0)
                throw ArcError::at(t.line, t.col,
                    "unterminated '{' in fmt string");

            if (!buf.empty()) {
                FmtPart p;
                p.isExpr = false;
                p.text   = buf;
                e->parts.push_back(std::move(p));
                buf.clear();
            }

            std::string exprText = raw.substr(i + 1, j - i - 1);
            {
                size_t a = 0, b = exprText.size();
                while (a < b && std::isspace(static_cast<unsigned char>(exprText[a]))) ++a;
                while (b > a && std::isspace(static_cast<unsigned char>(exprText[b - 1]))) --b;
                exprText = exprText.substr(a, b - a);
            }

            Lexer subLex(exprText);
            auto subToks = subLex.tokenize();
            for (auto& tt : subToks) { tt.line = t.line; tt.col = t.col; }

            auto savedToks = std::move(toks_);
            size_t savedI  = i_;
            toks_ = std::move(subToks);
            i_    = 0;

            ExprPtr subExpr;
            try {
                subExpr = parseExpr();
            } catch (...) {
                toks_ = std::move(savedToks);
                i_    = savedI;
                throw;
            }
            toks_ = std::move(savedToks);
            i_    = savedI;

            FmtPart p;
            p.isExpr = true;
            p.expr   = std::move(subExpr);
            e->parts.push_back(std::move(p));

            i = j + 1;
            continue;
        }

        if (c == '}' && i + 1 < raw.size() && raw[i + 1] == '}') {
            buf += '}';
            i += 2;
            continue;
        }

        buf += c;
        ++i;
    }
    if (!buf.empty()) {
        FmtPart p;
        p.isExpr = false;
        p.text   = buf;
        e->parts.push_back(std::move(p));
    }
    return e;
}

inline ExprPtr Parser::parseBlkExpr() {
    int ln = peek().line;
    adv();
    expect(TT::Colon, "':'");
    auto e = std::make_unique<BlkExpr>();
    e->line = ln;
    e->body = parseSuite();
    return e;
}

inline ExprPtr Parser::parseSelExpr() {
    int ln = peek().line;
    expect(TT::Kw_sel, "'sel'");
    auto cond = parseExpr();
    expect(TT::Comma, "','");
    auto tv = parseExpr();
    expect(TT::Comma, "','");
    auto fv = parseExpr();
    auto e = std::make_unique<SelExpr>(std::move(cond),
                                        std::move(tv),
                                        std::move(fv));
    e->line = ln;
    return e;
}

inline ExprPtr Parser::parseNamedInstr(const std::string& name, int line, int col) {
    auto e = std::make_unique<NamedInstrExpr>(name);
    e->line = line;
    e->col  = col;
    e->args.push_back(parseExpr());
    while (match(TT::Comma)) e->args.push_back(parseExpr());
    return e;
}

// ============================================================================
// P7.5  批次 4：编译期求值 + sext 最小执行
// ============================================================================

inline std::optional<int64_t> Parser::tryEvalConst(Expr* e, Program& prog) {
    if (!e) return std::nullopt;

    if (auto* ie = dynamic_cast<IntExpr*>(e)) return ie->v;
    if (auto* be = dynamic_cast<BolExpr*>(e)) return be->v ? 1 : 0;
    if (auto* ce = dynamic_cast<ChrExpr*>(e)) {
        return static_cast<int64_t>(static_cast<uint8_t>(ce->v));
    }
    if (auto* ue = dynamic_cast<UnaryExpr*>(e)) {
        auto v = tryEvalConst(ue->operand.get(), prog);
        if (!v) return std::nullopt;
        if (ue->op == "-") return -*v;
        if (ue->op == "~") return ~*v;
        if (ue->op == "!") return (*v == 0) ? 1 : 0;
        return std::nullopt;
    }
    if (auto* id = dynamic_cast<IdentExpr*>(e)) {
        return lookupConst(id->name);
    }
    if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
        auto l = tryEvalConst(b->lhs.get(), prog);
        auto r = tryEvalConst(b->rhs.get(), prog);
        if (!l || !r) return std::nullopt;
        if (b->op == "+")  return *l + *r;
        if (b->op == "-")  return *l - *r;
        if (b->op == "*")  return *l * *r;
        if (b->op == "/")  return (*r == 0) ? std::optional<int64_t>{} :
                                              std::optional<int64_t>(*l / *r);
        if (b->op == "%")  return (*r == 0) ? std::optional<int64_t>{} :
                                              std::optional<int64_t>(*l % *r);
        if (b->op == "==") return (*l == *r) ? 1 : 0;
        if (b->op == "!=") return (*l != *r) ? 1 : 0;
        if (b->op == "<")  return (*l <  *r) ? 1 : 0;
        if (b->op == ">")  return (*l >  *r) ? 1 : 0;
        if (b->op == "<=") return (*l <= *r) ? 1 : 0;
        if (b->op == ">=") return (*l >= *r) ? 1 : 0;
        if (b->op == "&&") return (*l && *r) ? 1 : 0;
        if (b->op == "||") return (*l || *r) ? 1 : 0;
        return std::nullopt;
    }
    if (auto* ce = dynamic_cast<CallExpr*>(e)) {
        if (auto* id = dynamic_cast<IdentExpr*>(ce->callee.get())) {
            auto fit = prog.compileTimeFunctions.find(id->name);
            if (fit != prog.compileTimeFunctions.end()) {
                std::vector<int64_t> args;
                for (auto& a : ce->args) {
                    auto v = tryEvalConst(a.get(), prog);
                    if (!v) return std::nullopt;
                    args.push_back(*v);
                }
                return evalCteCall(fit->second.get(), std::move(args), prog);
            }
        }
        return std::nullopt;
    }
    if (auto* cc = dynamic_cast<CteCallExpr*>(e)) {
        auto fit = prog.compileTimeFunctions.find(cc->fnName);
        if (fit == prog.compileTimeFunctions.end()) return std::nullopt;
        std::vector<int64_t> args;
        for (auto& a : cc->args) {
            auto v = tryEvalConst(a.get(), prog);
            if (!v) return std::nullopt;
            args.push_back(*v);
        }
        return evalCteCall(fit->second.get(), std::move(args), prog);
    }
    return std::nullopt;
}

inline std::optional<int64_t> Parser::evalCteCall(Function* fn,
                                                  std::vector<int64_t> args,
                                                  Program& prog) {
    if (!fn || !fn->hasBody) return std::nullopt;
    if (fn->params.size() != args.size()) return std::nullopt;

    std::unordered_map<std::string, int64_t> locals;
    for (size_t i = 0; i < args.size(); ++i)
        locals[fn->params[i].name] = args[i];

    std::function<std::optional<int64_t>(Expr*)> ev =
        [&](Expr* e) -> std::optional<int64_t> {
            if (!e) return std::nullopt;
            if (auto* ie = dynamic_cast<IntExpr*>(e)) return ie->v;
            if (auto* be = dynamic_cast<BolExpr*>(e)) return be->v ? 1 : 0;
            if (auto* ue = dynamic_cast<UnaryExpr*>(e)) {
                auto v = ev(ue->operand.get());
                if (!v) return std::nullopt;
                if (ue->op == "-") return -*v;
                if (ue->op == "~") return ~*v;
                if (ue->op == "!") return (*v == 0) ? 1 : 0;
                return std::nullopt;
            }
            if (auto* id = dynamic_cast<IdentExpr*>(e)) {
                auto it = locals.find(id->name);
                if (it != locals.end()) return it->second;
                return lookupConst(id->name);
            }
            if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
                auto l = ev(b->lhs.get());
                auto r = ev(b->rhs.get());
                if (!l || !r) return std::nullopt;
                if (b->op == "+") return *l + *r;
                if (b->op == "-") return *l - *r;
                if (b->op == "*") return *l * *r;
                if (b->op == "/") return (*r == 0) ? std::optional<int64_t>{} :
                                                      std::optional<int64_t>(*l / *r);
                if (b->op == "%") return (*r == 0) ? std::optional<int64_t>{} :
                                                      std::optional<int64_t>(*l % *r);
                if (b->op == "==") return (*l == *r) ? 1 : 0;
                if (b->op == "!=") return (*l != *r) ? 1 : 0;
                if (b->op == "<")  return (*l <  *r) ? 1 : 0;
                if (b->op == ">")  return (*l >  *r) ? 1 : 0;
                if (b->op == "<=") return (*l <= *r) ? 1 : 0;
                if (b->op == ">=") return (*l >= *r) ? 1 : 0;
                return std::nullopt;
            }
            if (auto* ce = dynamic_cast<CallExpr*>(e)) {
                if (auto* id = dynamic_cast<IdentExpr*>(ce->callee.get())) {
                    auto fit = prog.compileTimeFunctions.find(id->name);
                    if (fit != prog.compileTimeFunctions.end()) {
                        std::vector<int64_t> args;
                        for (auto& a : ce->args) {
                            auto v = ev(a.get());
                            if (!v) return std::nullopt;
                            args.push_back(*v);
                        }
                        return evalCteCall(fit->second.get(),
                                           std::move(args), prog);
                    }
                }
                return std::nullopt;
            }
            if (auto* cc = dynamic_cast<CteCallExpr*>(e)) {
                auto fit = prog.compileTimeFunctions.find(cc->fnName);
                if (fit == prog.compileTimeFunctions.end()) return std::nullopt;
                std::vector<int64_t> args;
                for (auto& a : cc->args) {
                    auto v = ev(a.get());
                    if (!v) return std::nullopt;
                    args.push_back(*v);
                }
                return evalCteCall(fit->second.get(), std::move(args), prog);
            }
            return std::nullopt;
        };

    std::function<std::optional<int64_t>(const std::vector<StmtPtr>&)> execBody;
    execBody = [&](const std::vector<StmtPtr>& body) -> std::optional<int64_t> {
        for (auto& s : body) {
            if (auto* vd = dynamic_cast<VarDeclStmt*>(s.get())) {
                for (auto& it : vd->vars) locals[it.name] = 0;
                continue;
            }
            if (auto* as = dynamic_cast<AssignStmt*>(s.get())) {
                auto* id = dynamic_cast<IdentExpr*>(as->target.get());
                if (!id) return std::nullopt;
                auto v = ev(as->value.get());
                if (!v) return std::nullopt;
                locals[id->name] = *v;
                continue;
            }
            if (auto* inc = dynamic_cast<IncDecStmt*>(s.get())) {
                auto* id = dynamic_cast<IdentExpr*>(inc->target.get());
                if (!id) return std::nullopt;
                auto it = locals.find(id->name);
                if (it == locals.end()) return std::nullopt;
                it->second += inc->isInc ? 1 : -1;
                continue;
            }
            if (auto* rs = dynamic_cast<RetStmt*>(s.get())) {
                if (!rs->value) return int64_t(0);
                return ev(rs->value.get());
            }
            if (auto* is = dynamic_cast<IfStmt*>(s.get())) {
                auto cv = ev(is->cond.get());
                if (!cv) return std::nullopt;
                const auto& branch = (*cv != 0) ? is->thenBody : is->elseBody;
                auto r = execBody(branch);
                if (r) return r;
                continue;
            }
            if (auto* ws = dynamic_cast<WhileStmt*>(s.get())) {
                int64_t guard = 0;
                while (true) {
                    if (++guard > 1000000) return std::nullopt;
                    auto cv = ev(ws->cond.get());
                    if (!cv) return std::nullopt;
                    if (*cv == 0) break;
                    auto r = execBody(ws->body);
                    if (r) return r;
                }
                continue;
            }
        }
        return std::nullopt;
    };

    return execBody(fn->body);
}

inline void mergeProgram(Program& dst, Program& src) {
    for (auto& kv : src.functions) dst.functions[kv.first] = kv.second;
    for (auto& kv : src.compileTimeFunctions) dst.compileTimeFunctions[kv.first] = kv.second;
    for (auto& kv : src.records) dst.records[kv.first] = kv.second;
    for (auto& kv : src.enums) dst.enums[kv.first] = kv.second;
    for (auto& kv : src.unions) dst.unions[kv.first] = kv.second;
    for (auto& kv : src.classes) dst.classes[kv.first] = kv.second;
    for (auto& kv : src.traits) dst.traits[kv.first] = kv.second;
    for (auto& kv : src.traitsByType) {
        auto& v = dst.traitsByType[kv.first];
        for (auto& t : kv.second) v.push_back(t);
    }
    for (auto& kv : src.modules) dst.modules[kv.first] = kv.second;
    for (auto& kv : src.typedefs) dst.typedefs[kv.first] = kv.second;
    for (auto& kv : src.sigs) dst.sigs[kv.first] = kv.second;
    for (auto& kv : src.consts) dst.consts[kv.first] = kv.second;
    for (auto& kv : src.useAliases) dst.useAliases[kv.first] = kv.second;
    if (!dst.entry && src.entry) dst.entry = src.entry;
    for (auto& tl : src.topLevel) dst.topLevel.push_back(std::move(tl));
}

inline std::vector<std::string> collectPatternNames(const PatternPtr& p) {
    std::vector<std::string> out;
    if (!p) return out;
    if (p->kind == Pattern::Bind) {
        out.push_back(p->name);
    } else if (p->kind == Pattern::TupPattern) {
        for (auto& sub : p->args) {
            auto names = collectPatternNames(sub);
            out.insert(out.end(), names.begin(), names.end());
        }
    } else if (p->kind == Pattern::Alias) {
        out.push_back(p->name);
        auto names = collectPatternNames(p->aliasInner);
        out.insert(out.end(), names.begin(), names.end());
    }
    return out;
}

inline void Parser::execSextBlock(SextBlock* blk, Program& prog) {
    if (!blk) return;
    for (auto& s : blk->stmts) execSextStmt(s, prog);
}

inline void Parser::execSextStmt(SextStmt& s, Program& prog) {
    switch (s.kind) {
        case SextStmt::Equ: {
            auto v = tryEvalConst(s.expr.get(), prog);
            if (v) consts_[s.name] = *v;
            return;
        }
        case SextStmt::Set: {
            auto v = tryEvalConst(s.expr.get(), prog);
            if (v) consts_[s.name] = *v;
            return;
        }
        case SextStmt::If: {
            auto v = tryEvalConst(s.expr.get(), prog);
            bool cond = v.has_value() && *v != 0;
            if (cond) {
                for (auto& inner : s.body)     execSextStmt(inner, prog);
            } else {
                for (auto& inner : s.elseBody) execSextStmt(inner, prog);
            }
            return;
        }
        case SextStmt::Rep: {
            auto v = tryEvalConst(s.expr.get(), prog);
            if (!v || *v < 0) return;
            for (int64_t i = 0; i < *v; ++i)
                for (auto& inner : s.body) execSextStmt(inner, prog);
            return;
        }
        case SextStmt::For: {
            auto a = tryEvalConst(s.arg1.get(), prog);
            auto b = tryEvalConst(s.arg2.get(), prog);
            if (!a || !b) return;
            int64_t saved = consts_.count(s.name) ? consts_[s.name] : 0;
            for (int64_t i = *a; i <= *b; ++i) {
                consts_[s.name] = i;
                for (auto& inner : s.body) execSextStmt(inner, prog);
            }
            consts_[s.name] = saved;
            return;
        }
        case SextStmt::Gen:
            std::cerr << "[sext.gen] " << s.text << "\n";
            return;
        case SextStmt::CteCall: {
            auto fit = prog.compileTimeFunctions.find(s.name);
            if (fit == prog.compileTimeFunctions.end()) return;
            std::vector<int64_t> args;
            for (auto& a : s.cteArgs) {
                auto v = tryEvalConst(a.get(), prog);
                args.push_back(v.has_value() ? *v : 0);
            }
            (void)evalCteCall(fit->second.get(), std::move(args), prog);
            return;
        }
        case SextStmt::Alc: {
            if (s.text == "code") {
                std::cerr << "[sext.alc] " << s.name
                          << " (code mode, no-op)\n";
                return;
            }
            auto v = tryEvalConst(s.arg1.get(), prog);
            if (v) consts_[s.name] = *v;
            return;
        }
        case SextStmt::Inc:
        case SextStmt::Lod: {
            if (s.text.empty() || !curProg_) return;
            std::ifstream f(s.text);
            if (!f) {
                std::cerr << "[sext.inc/lod] cannot open " << s.text << "\n";
                return;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            Lexer subLex(ss.str());
            auto subToks = subLex.tokenize();
            Parser subParser(std::move(subToks));
            subParser.consts_ = consts_;
            subParser.moduleStack_ = moduleStack_;
            auto subProg = subParser.parseProgram();
            for (auto& kv : consts_) subProg->consts[kv.first] = kv.second;
            mergeProgram(*curProg_, *subProg);
            return;
        }
        case SextStmt::Upd: {
            std::cerr << "[sext.upd] " << s.name << " (no-op)\n";
            return;
        }
        case SextStmt::DefMac: {
            if (!s.arg1) return;
            std::vector<std::string> params =
                collectPatternNames(s.macroPattern);
            if (params.empty()) return;

            auto fn = std::make_shared<Function>();
            fn->name = s.name;
            fn->isCompileTime = true;
            fn->line = s.line;
            for (auto& pn : params) {
                Param p;
                p.name = pn;
                p.type = Type::base(Type::Int);
                fn->params.push_back(std::move(p));
            }
            auto ret = std::make_unique<RetStmt>();
            ret->line  = s.line;
            ret->value = std::move(s.arg1);
            fn->body.push_back(std::move(ret));
            fn->hasBody = true;

            prog.compileTimeFunctions[s.name] = fn;
            return;
        }
        case SextStmt::DefAttr:
            return;
        default:
            return;
    }
}

// P7 结束。namespace arc 保持打开。
// ============================================================================
// P8  运行时 Value / Env / Object / Array / Map / Opt / Rlt
// ============================================================================

struct Value;
struct Array;
struct Object;
struct MapValue;
struct Closure;
struct RltValue;
struct RangeValue;
struct Coroutine;
struct OptValue;
struct SliceValue;
struct Env;
struct PtrValue;

using ArrayPtr     = std::shared_ptr<Array>;
using ObjectPtr    = std::shared_ptr<Object>;
using MapPtr       = std::shared_ptr<MapValue>;
using ClosurePtr   = std::shared_ptr<Closure>;
using RltPtr       = std::shared_ptr<RltValue>;
using RangePtr     = std::shared_ptr<RangeValue>;
using CoroutinePtr = std::shared_ptr<Coroutine>;
using OptPtr       = std::shared_ptr<OptValue>;
using SlicePtr     = std::shared_ptr<SliceValue>;
using EnvPtr       = std::shared_ptr<Env>;
using PtrPtr       = std::shared_ptr<PtrValue>;

struct PtrValue {
    Value* target = nullptr;
    bool   owned  = false;
};

struct Value {
    using Var = std::variant<
        std::monostate,
        int64_t,
        double,
        bool,
        char,
        std::string,
        ArrayPtr,
        ObjectPtr,
        MapPtr,
        ClosurePtr,
        RltPtr,
        RangePtr,
        CoroutinePtr,
        OptPtr,
        SlicePtr,
        PtrPtr
    >;

    Var v;

    Value()                   : v(std::monostate{}) {}
    Value(std::monostate m)   : v(m) {}
    Value(int64_t x)          : v(x) {}
    Value(int x)              : v(int64_t(x)) {}
    Value(double x)           : v(x) {}
    Value(bool x)             : v(x) {}
    Value(char x)             : v(x) {}
    Value(std::string s)      : v(std::move(s)) {}
    Value(const char* s)      : v(std::string(s)) {}
    Value(ArrayPtr a)         : v(std::move(a)) {}
    Value(ObjectPtr o)        : v(std::move(o)) {}
    Value(MapPtr m)           : v(std::move(m)) {}
    Value(ClosurePtr c)       : v(std::move(c)) {}
    Value(RltPtr r)           : v(std::move(r)) {}
    Value(RangePtr r)         : v(std::move(r)) {}
    Value(CoroutinePtr c)     : v(std::move(c)) {}
    Value(OptPtr o)           : v(std::move(o)) {}
    Value(SlicePtr s)         : v(std::move(s)) {}
    Value(PtrPtr p)           : v(std::move(p)) {}
};

inline bool isNil      (const Value& v) { return std::holds_alternative<std::monostate>(v.v); }
inline bool isInt      (const Value& v) { return std::holds_alternative<int64_t>(v.v); }
inline bool isFlt      (const Value& v) { return std::holds_alternative<double>(v.v); }
inline bool isBol      (const Value& v) { return std::holds_alternative<bool>(v.v); }
inline bool isChr      (const Value& v) { return std::holds_alternative<char>(v.v); }
inline bool isStr      (const Value& v) { return std::holds_alternative<std::string>(v.v); }
inline bool isArr      (const Value& v) { return std::holds_alternative<ArrayPtr>(v.v); }
inline bool isObj      (const Value& v) { return std::holds_alternative<ObjectPtr>(v.v); }
inline bool isMap      (const Value& v) { return std::holds_alternative<MapPtr>(v.v); }
inline bool isClosure  (const Value& v) { return std::holds_alternative<ClosurePtr>(v.v); }
inline bool isRlt      (const Value& v) { return std::holds_alternative<RltPtr>(v.v); }
inline bool isRange    (const Value& v) { return std::holds_alternative<RangePtr>(v.v); }
inline bool isCoroutine(const Value& v) { return std::holds_alternative<CoroutinePtr>(v.v); }
inline bool isOpt      (const Value& v) { return std::holds_alternative<OptPtr>(v.v); }
inline bool isSlice    (const Value& v) { return std::holds_alternative<SlicePtr>(v.v); }
inline bool isPtr      (const Value& v) { return std::holds_alternative<PtrPtr>(v.v); }

inline bool isNum(const Value& v) {
    return isInt(v) || isFlt(v) || isChr(v) || isBol(v);
}
inline bool isIntegral(const Value& v) {
    return isInt(v) || isChr(v) || isBol(v);
}

inline int64_t asInt(const Value& v) {
    if (isInt(v)) return std::get<int64_t>(v.v);
    if (isChr(v)) return int64_t(static_cast<uint8_t>(std::get<char>(v.v)));
    if (isBol(v)) return std::get<bool>(v.v) ? 1 : 0;
    throw ArcError("expected integer value");
}
inline double asFlt(const Value& v) {
    if (isFlt(v)) return std::get<double>(v.v);
    if (isInt(v)) return double(std::get<int64_t>(v.v));
    if (isChr(v)) return double(static_cast<uint8_t>(std::get<char>(v.v)));
    if (isBol(v)) return std::get<bool>(v.v) ? 1.0 : 0.0;
    throw ArcError("expected numeric value");
}
inline bool asBol(const Value& v) {
    if (isBol(v)) return std::get<bool>(v.v);
    if (isInt(v)) return std::get<int64_t>(v.v) != 0;
    if (isChr(v)) return static_cast<uint8_t>(std::get<char>(v.v)) != 0;
    throw ArcError("expected boolean value");
}
inline uint8_t asChr(const Value& v) {
    if (isChr(v)) return static_cast<uint8_t>(std::get<char>(v.v));
    if (isInt(v)) return static_cast<uint8_t>(std::get<int64_t>(v.v) & 0xFF);
    throw ArcError("expected character value");
}
inline const std::string& asStr(const Value& v) {
    if (!isStr(v)) throw ArcError("expected string value");
    return std::get<std::string>(v.v);
}

struct Array {
    std::vector<Value> elems;
    Array() = default;
    explicit Array(std::vector<Value> v) : elems(std::move(v)) {}
};

struct Object {
    std::string className;
    std::map<std::string, Value> fields;
    Object() = default;
    explicit Object(std::string n) : className(std::move(n)) {}
};

struct MapKey {
    std::variant<int64_t, uint8_t, bool, std::string> k;

    MapKey() : k(int64_t(0)) {}
    explicit MapKey(int64_t i) : k(i) {}
    explicit MapKey(uint8_t c) : k(c) {}
    explicit MapKey(bool b)    : k(b) {}
    explicit MapKey(std::string s) : k(std::move(s)) {}

    bool operator==(const MapKey& o) const { return k == o.k; }
};

struct MapKeyHash {
    size_t operator()(const MapKey& mk) const {
        return std::visit([](auto&& x) -> size_t {
            using T = std::decay_t<decltype(x)>;
            return std::hash<T>{}(x);
        }, mk.k);
    }
};

struct MapValue {
    std::unordered_map<MapKey, Value, MapKeyHash> data;
};

struct Closure {
    std::shared_ptr<Function> fn;
    EnvPtr env;
    Closure() = default;
    Closure(std::shared_ptr<Function> f, EnvPtr e)
        : fn(std::move(f)), env(std::move(e)) {}
};

struct RltValue {
    bool  isOk;
    Value value;
    RltValue(bool ok, Value v) : isOk(ok), value(std::move(v)) {}
};

struct SliceValue {
    ArrayPtr source;
    int64_t  offset = 0;
    int64_t  length = 0;
};

struct OptValue {
    bool  has = false;
    Value value;
    OptValue() = default;
    OptValue(bool h, Value v) : has(h), value(std::move(v)) {}
};

struct RangeValue {
    Value from, to;
    bool  exclusive = false;
    bool  hasStep   = false;
    Value step;
};

struct CoFrame {
    enum Kind { Block, WhileHeader, ForRangeHeader, ForCHeader } kind = Block;
    bool cIncrPending = false;

    const std::vector<StmtPtr>* body = nullptr;
    size_t pc = 0;
    EnvPtr env;

    Expr* cond = nullptr;
    const std::vector<StmtPtr>* loopBody = nullptr;
    std::string loopLabel;

    std::string varName;
    bool exclusive = false;
    bool useFloat = false;
    int64_t i64 = 0, i64End = 0, i64Step = 1;
    double  f64 = 0, f64End = 0, f64Step = 1;
    const std::vector<StmtPtr>* forBody = nullptr;

    Expr*  cCond = nullptr;
    Stmt*  cIncr = nullptr;
    const std::vector<StmtPtr>* cBody = nullptr;
};

struct Coroutine {
    enum State { New, Suspended, Running, Done, Failed } state = New;

    std::shared_ptr<Function> fn;
    EnvPtr env;
    std::vector<CoFrame> stack;

    Value yieldValue;
    Value returnValue;
    std::exception_ptr err;
};

struct Env {
    std::map<std::string, Value> vars;
    std::unordered_set<std::string> moved;
    std::unordered_set<std::string> consts;
    std::unordered_set<std::string> owned;
    std::unordered_set<std::string> noRelease;
    EnvPtr parent;
    EnvPtr globals;

    Env() = default;
    explicit Env(EnvPtr p) : parent(std::move(p)) {
        globals = parent ? parent->globals : nullptr;
    }

    Value* find(const std::string& n) {
        Env* e = this;
        while (e) {
            auto it = e->vars.find(n);
            if (it != e->vars.end()) return &it->second;
            e = e->parent.get();
        }
        return nullptr;
    }

    void define(const std::string& n, Value v) {
        vars[n] = std::move(v);
    }

    bool isMoved(const std::string& n) const {
        const Env* e = this;
        while (e) {
            if (e->moved.count(n)) return true;
            if (e->vars.count(n)) return false;
            e = e->parent.get();
        }
        return false;
    }

    void markMoved(const std::string& n) {
        Env* e = this;
        while (e) {
            if (e->vars.count(n)) { e->moved.insert(n); return; }
            e = e->parent.get();
        }
    }

    void clearMoved(const std::string& n) {
        Env* e = this;
        while (e) {
            if (e->moved.count(n)) e->moved.erase(n);
            if (e->vars.count(n)) return;
            e = e->parent.get();
        }
    }

    bool isConst(const std::string& n) const {
        const Env* e = this;
        while (e) {
            if (e->consts.count(n)) return true;
            if (e->vars.count(n)) return false;
            e = e->parent.get();
        }
        return false;
    }

    void markConst(const std::string& n) {
        Env* e = this;
        while (e) {
            if (e->vars.count(n)) { e->consts.insert(n); return; }
            e = e->parent.get();
        }
    }
};

inline Value defaultOfType(const TypePtr& t);

inline Value defaultOfType(const TypePtr& t) {
    if (!t) return Value{};

    switch (t->kind) {
        case Type::Int:   return Value(int64_t(0));
        case Type::Flt:   return Value(0.0);
        case Type::Chr:   return Value(char(0));
        case Type::Bol:   return Value(false);
        case Type::Str:   return Value(std::string());
        case Type::Nil:   return Value{};

        case Type::Arr: {
            auto arr = std::make_shared<Array>();
            int64_t n = t->arraySize;
            TypePtr elemTy = t->args.empty() ? nullptr : t->args[0];
            arr->elems.reserve(static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i)
                arr->elems.push_back(defaultOfType(elemTy));
            return Value(arr);
        }

        case Type::Tup: {
            std::vector<Value> elems;
            elems.reserve(t->args.size());
            for (auto& sub : t->args)
                elems.push_back(defaultOfType(sub));
            return Value(std::make_shared<Array>(std::move(elems)));
        }

        case Type::Map:
            return Value(std::make_shared<MapValue>());

        case Type::Opt:
            return Value(std::make_shared<OptValue>(false, Value{}));

        case Type::Rlt: {
            TypePtr okTy = t->args.empty() ? nullptr : t->args[0];
            return Value(std::make_shared<RltValue>(true, defaultOfType(okTy)));
        }

        case Type::Rng: {
            auto r = std::make_shared<RangeValue>();
            r->from = Value(int64_t(0));
            r->to   = Value(int64_t(0));
            return Value(r);
        }

        case Type::Ptr:
        case Type::OwnPtr:
        case Type::Cod:
            return Value{};

        case Type::Sig:
        case Type::Fun:
            return Value{};

        case Type::Tck:
            return Value{};

        case Type::Slc: {
            auto sv = std::make_shared<SliceValue>();
            sv->source = std::make_shared<Array>();
            return Value(sv);
        }

        case Type::Named:
            return Value(std::make_shared<Object>(t->name));

        default:
            return Value{};
    }
}

inline std::string valueToString(const Value& v);

inline std::string arrayToString(const ArrayPtr& a) {
    std::string s = "[";
    for (size_t i = 0; i < a->elems.size(); ++i) {
        if (i) s += ", ";
        s += valueToString(a->elems[i]);
    }
    s += "]";
    return s;
}

inline std::string mapToString(const MapPtr& m) {
    std::string s = "{";
    bool first = true;
    for (auto& kv : m->data) {
        if (!first) s += ", ";
        first = false;
        std::visit([&s](auto&& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::string>) {
                s += x;
            } else if constexpr (std::is_same_v<T, uint8_t>) {
                s += std::string(1, static_cast<char>(x));
            } else if constexpr (std::is_same_v<T, bool>) {
                s += (x ? "tru" : "fal");
            } else {
                s += std::to_string(x);
            }
        }, kv.first.k);
        s += ": ";
        s += valueToString(kv.second);
    }
    s += "}";
    return s;
}

inline std::string objectToString(const ObjectPtr& o) {
    std::string s = o->className + "{";
    bool first = true;
    for (auto& kv : o->fields) {
        if (!first) s += ", ";
        first = false;
        s += kv.first + "=" + valueToString(kv.second);
    }
    s += "}";
    return s;
}

inline std::string valueToString(const Value& v) {
    if (isNil(v))  return "nil";
    if (isInt(v))  return std::to_string(std::get<int64_t>(v.v));
    if (isFlt(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v.v);
        return ss.str();
    }
    if (isBol(v))  return std::get<bool>(v.v) ? "tru" : "fal";
    if (isChr(v))  return std::string(1, static_cast<char>(
                       static_cast<uint8_t>(std::get<char>(v.v))));
    if (isStr(v))  return std::get<std::string>(v.v);
    if (isArr(v))  return arrayToString(std::get<ArrayPtr>(v.v));
    if (isSlice(v)) {
        auto sl = std::get<SlicePtr>(v.v);
        std::string s = "[";
        for (int64_t i = 0; i < sl->length; ++i) {
            if (i) s += ", ";
            s += valueToString(sl->source->elems[sl->offset + i]);
        }
        s += "]";
        return s;
    }
    if (isObj(v))  return objectToString(std::get<ObjectPtr>(v.v));
    if (isMap(v))  return mapToString(std::get<MapPtr>(v.v));
    if (isClosure(v)) {
        auto c = std::get<ClosurePtr>(v.v);
        if (c && c->fn) return "<fn " + c->fn->name + ">";
        return "<fn>";
    }
    if (isRlt(v)) {
        auto r = std::get<RltPtr>(v.v);
        if (!r) return "nil";
        return r->isOk ? ("ok(" + valueToString(r->value) + ")")
                       : ("er(" + valueToString(r->value) + ")");
    }
    if (isRange(v)) {
        auto r = std::get<RangePtr>(v.v);
        if (!r) return "rng<?>";
        std::string s = "rng " + valueToString(r->from)
                      + (r->exclusive ? "..<" : "..")
                      + valueToString(r->to);
        if (r->hasStep) s += " step " + valueToString(r->step);
        return s;
    }
    if (isCoroutine(v)) return "<coroutine>";
    if (isOpt(v)) {
        auto o = std::get<OptPtr>(v.v);
        if (!o || !o->has) return "nil";
        return valueToString(o->value);
    }
    if (isPtr(v)) {
        auto p = std::get<PtrPtr>(v.v);
        if (!p || !p->target) return "<nullptr>";
        return "<ptr>";
    }
    return "?";
}

inline bool valueEquals(const Value& a, const Value& b) {
    if (isNil(a) && isNil(b)) return true;

    if (isChr(a) && isChr(b)) return asChr(a) == asChr(b);

    if (isInt(a) && isInt(b)) return std::get<int64_t>(a.v) == std::get<int64_t>(b.v);
    if (isFlt(a) && isFlt(b)) return std::get<double>(a.v) == std::get<double>(b.v);
    if (isBol(a) && isBol(b)) return std::get<bool>(a.v) == std::get<bool>(b.v);
    if (isStr(a) && isStr(b)) return std::get<std::string>(a.v) == std::get<std::string>(b.v);

    if (isNum(a) && isNum(b)) return asFlt(a) == asFlt(b);

    if (isArr(a) && isArr(b)) {
        auto x = std::get<ArrayPtr>(a.v);
        auto y = std::get<ArrayPtr>(b.v);
        if (!x || !y) return x == y;
        if (x->elems.size() != y->elems.size()) return false;
        for (size_t i = 0; i < x->elems.size(); ++i)
            if (!valueEquals(x->elems[i], y->elems[i])) return false;
        return true;
    }
    if (isSlice(a) && isSlice(b)) {
        auto x = std::get<SlicePtr>(a.v);
        auto y = std::get<SlicePtr>(b.v);
        if (!x || !y) return x == y;
        if (x->length != y->length) return false;
        for (int64_t i = 0; i < x->length; ++i)
            if (!valueEquals(x->source->elems[x->offset + i],
                             y->source->elems[y->offset + i])) return false;
        return true;
    }
    if (isObj(a) && isObj(b)) return std::get<ObjectPtr>(a.v) == std::get<ObjectPtr>(b.v);
    if (isMap(a) && isMap(b)) return std::get<MapPtr>(a.v) == std::get<MapPtr>(b.v);
    if (isClosure(a) && isClosure(b)) return std::get<ClosurePtr>(a.v) == std::get<ClosurePtr>(b.v);
    if (isRlt(a) && isRlt(b)) {
        auto x = std::get<RltPtr>(a.v);
        auto y = std::get<RltPtr>(b.v);
        if (!x || !y) return x == y;
        if (x->isOk != y->isOk) return false;
        return valueEquals(x->value, y->value);
    }
    if (isOpt(a) && isOpt(b)) {
        auto x = std::get<OptPtr>(a.v);
        auto y = std::get<OptPtr>(b.v);
        if (!x || !y) return x == y;
        if (x->has != y->has) return false;
        if (!x->has) return true;
        return valueEquals(x->value, y->value);
    }
    if (isPtr(a) && isPtr(b)) {
        auto x = std::get<PtrPtr>(a.v);
        auto y = std::get<PtrPtr>(b.v);
        if (!x || !y) return x == y;
        return x->target == y->target;
    }
    return false;
}

inline bool truthy(const Value& v) {
    if (isBol(v)) return std::get<bool>(v.v);
    if (isInt(v)) return std::get<int64_t>(v.v) != 0;
    if (isFlt(v)) return std::get<double>(v.v) != 0.0;
    if (isChr(v)) return asChr(v) != 0;
    if (isStr(v)) return !std::get<std::string>(v.v).empty();
    if (isNil(v)) return false;
    if (isOpt(v)) {
        auto o = std::get<OptPtr>(v.v);
        return o && o->has;
    }
    return true;
}

inline MapKey valueToMapKey(const Value& v) {
    if (isInt(v))  return MapKey(std::get<int64_t>(v.v));
    if (isChr(v))  return MapKey(asChr(v));
    if (isBol(v))  return MapKey(std::get<bool>(v.v));
    if (isStr(v))  return MapKey(std::get<std::string>(v.v));
    throw ArcError("map key must be int / chr / bol / str");
}

inline MapKey valueToMapKeyAt(const Value& v, int line, int col) {
    if (isInt(v) || isChr(v) || isBol(v) || isStr(v))
        return valueToMapKey(v);
    std::string kind = "unknown";
    if (isNil(v))       kind = "nil";
    else if (isFlt(v))  kind = "flt";
    else if (isArr(v))  kind = "arr";
    else if (isObj(v))  kind = "obj";
    else if (isMap(v))  kind = "map";
    else if (isClosure(v)) kind = "closure";
    else if (isRlt(v))  kind = "rlt";
    else if (isRange(v)) kind = "rng";
    else if (isCoroutine(v)) kind = "tck";
    else if (isOpt(v))  kind = "opt";
    else if (isSlice(v)) kind = "slc";
    throw ArcError::at(line, col,
        "map key must be int / chr / bol / str, got " + kind);
}

// ============================================================================
// P9  Interpreter —— 表达式求值
// ============================================================================

struct ReturnSignal    { Value value; };
struct BreakSignal     { std::string label; };
struct ContinueSignal  { std::string label; };
struct TryReturnSignal { Value error; };

class Interpreter {
public:
    explicit Interpreter(std::shared_ptr<Program> prog)
        : prog_(std::move(prog)) {}

    int run(int argc = 0, char** argv = nullptr);

public:
    std::shared_ptr<Program> prog_;
    EnvPtr global_;
    std::unordered_map<std::string, EnvPtr> moduleEnvs_;
    //C++ 11 doesn't allow recursive lambdas, so we use a member function for expression evaluation.AI写的这个注释 反正我C++11写不了这个 这是我写的唯一的注释
    Value evalExpr(Expr* e, EnvPtr env);
    Value evalUnary(const std::string& op, const Value& operand);
    Value evalBinaryExpr(BinaryExpr* b, EnvPtr env);
    Value evalNamedInstr(NamedInstrExpr* e, EnvPtr env);
    Value evalCall(CallExpr* e, EnvPtr env);
    Value evalMethodCall(MethodCallExpr* e, EnvPtr env);
    Value evalCalCall(CalCallExpr* e, EnvPtr env);
    Value evalIndex(IndexExpr* e, EnvPtr env);
    Value evalSlice(SliceExpr* e, EnvPtr env);
    Value evalMember(MemberExpr* e, EnvPtr env);
    Value evalTupleIndex(TupleIndexExpr* e, EnvPtr env);
    Value evalOptHas(OptHasExpr* e, EnvPtr env);
    Value evalOptVal(OptValExpr* e, EnvPtr env);
    Value evalAsConv(AsConvExpr* e, EnvPtr env);
    Value evalOk(OkExpr* e, EnvPtr env);
    Value evalEr(ErExpr* e, EnvPtr env);
    Value evalTry(TryExpr* e, EnvPtr env);
    Value evalUnw(UnwExpr* e, EnvPtr env);
    Value evalOr(OrExpr* e, EnvPtr env);
    Value evalArrayLit(ArrayLitExpr* e, EnvPtr env);
    Value evalTupleLit(TupleLitExpr* e, EnvPtr env);
    Value evalRecLit(RecLitExpr* e, EnvPtr env);
    Value evalClsLit(ClsLitExpr* e, EnvPtr env);
    Value evalEnmLit(EnmLitExpr* e, EnvPtr env);
    Value evalRangeLit(RangeLitExpr* e, EnvPtr env);
    Value evalSizeof(SizeofExpr* e, EnvPtr env);
    Value evalSel(SelExpr* e, EnvPtr env);
    Value evalBlk(BlkExpr* e, EnvPtr env);
    Value evalLambda(LamExpr* e, EnvPtr env);
    Value evalFmt(FmtExpr* e, EnvPtr env);
    Value evalPipe(PipeExpr* e, EnvPtr env);
    Value evalRse(RseExpr* e, EnvPtr env);

    void execStmt(Stmt* s, EnvPtr env);
    void execSuite(const std::vector<StmtPtr>& body, EnvPtr env);
    void execBlock(const std::vector<StmtPtr>& body, EnvPtr env);

    Value callFunction(std::shared_ptr<Function> fn,
                       std::vector<Value> args,
                       EnvPtr closureEnv);

    void assignTo(Expr* target, const Value& val, EnvPtr env);
    Value* lvalueOf(Expr* target, EnvPtr env);

    Value startCoroutine(std::shared_ptr<Function> fn,
                         std::vector<Value> args,
                         EnvPtr env);
    Value resumeCoroutine(CoroutinePtr co, const Value& in);

    bool matchPattern(const PatternPtr& p, const Value& v,
                      std::unordered_map<std::string, Value>& bindings);

    FunctionPtr lookupMethod(const std::string& className,
                             const std::string& method);

    void destroyOwnedInScope(EnvPtr env);
};

struct ScopeGuard {
    Interpreter* ip;
    EnvPtr env;
    ScopeGuard(Interpreter* i, EnvPtr e) : ip(i), env(e) {}
    ~ScopeGuard() { if (ip && env) ip->destroyOwnedInScope(env); }
};

inline Value applyBinary(const std::string& op,
                         const Value& l, const Value& r) {
    if (isArr(l) && isArr(r)) {
        auto la = std::get<ArrayPtr>(l.v);
        auto ra = std::get<ArrayPtr>(r.v);
        if (la->elems.size() != ra->elems.size()) {
            bool isCmp = (op == "==" || op == "!=" ||
                          op == "<"  || op == ">"  ||
                          op == "<=" || op == ">=");
            throw ArcError(std::string("array ")
                + (isCmp ? "comparison" : "arithmetic")
                + " size mismatch");
        }
        std::vector<Value> out;
        out.reserve(la->elems.size());
        for (size_t i = 0; i < la->elems.size(); ++i)
            out.push_back(applyBinary(op, la->elems[i], ra->elems[i]));
        return Value(std::make_shared<Array>(std::move(out)));
    }

    if (isArr(l) || isArr(r)) {
        if (isArr(l)) {
            auto la = std::get<ArrayPtr>(l.v);
            std::vector<Value> out;
            out.reserve(la->elems.size());
            for (size_t i = 0; i < la->elems.size(); ++i)
                out.push_back(applyBinary(op, la->elems[i], r));
            return Value(std::make_shared<Array>(std::move(out)));
        }
        auto ra = std::get<ArrayPtr>(r.v);
        std::vector<Value> out;
        out.reserve(ra->elems.size());
        for (size_t i = 0; i < ra->elems.size(); ++i)
            out.push_back(applyBinary(op, l, ra->elems[i]));
        return Value(std::make_shared<Array>(std::move(out)));
    }

    if (op == "==") return Value(valueEquals(l, r));
    if (op == "!=") return Value(!valueEquals(l, r));

    if (isStr(l) && isStr(r)) {
        const auto& a = asStr(l);
        const auto& b = asStr(r);
        if (op == "+")  return Value(a + b);
        if (op == "<")  return Value(a < b);
        if (op == ">")  return Value(a > b);
        if (op == "<=") return Value(a <= b);
        if (op == ">=") return Value(a >= b);
        throw ArcError("invalid string operator: " + op);
    }

    if (isIntegral(l) && isIntegral(r)) {
        int64_t a = asInt(l);
        int64_t b = asInt(r);
        if (op == "+") return Value(a + b);
        if (op == "-") return Value(a - b);
        if (op == "*") return Value(a * b);
        if (op == "/") {
            if (b == 0) throw ArcError("division by zero");
            return Value(a / b);
        }
        if (op == "%") {
            if (b == 0) throw ArcError("modulo by zero");
            return Value(a % b);
        }
        if (op == "^") {
            if (b < 0)
                return Value(std::pow(static_cast<double>(a),
                                      static_cast<double>(b)));
            int64_t result = 1;
            int64_t base = a;
            int64_t exp  = b;
            while (exp > 0) {
                if (exp & 1) result *= base;
                base *= base;
                exp >>= 1;
            }
            return Value(result);
        }
        if (op == "<")  return Value(a < b);
        if (op == ">")  return Value(a > b);
        if (op == "<=") return Value(a <= b);
        if (op == ">=") return Value(a >= b);
        if (op == "&")  return Value(a & b);
        if (op == "|")  return Value(a | b);
        if (op == "<<") return Value(a << b);
        if (op == ">>") return Value(a >> b);
        if (op == ">>>") {
            return Value(static_cast<int64_t>(
                static_cast<uint64_t>(a) >> b));
        }
        throw ArcError("invalid integer operator: " + op);
    }

    if (isNum(l) && isNum(r)) {
        double a = asFlt(l);
        double b = asFlt(r);
        if (op == "+") return Value(a + b);
        if (op == "-") return Value(a - b);
        if (op == "*") return Value(a * b);
        if (op == "/") return Value(a / b);
        if (op == "%") return Value(std::fmod(a, b));
        if (op == "^") return Value(std::pow(a, b));
        if (op == "<")  return Value(a < b);
        if (op == ">")  return Value(a > b);
        if (op == "<=") return Value(a <= b);
        if (op == ">=") return Value(a >= b);
    }

    throw ArcError("invalid operands for operator: " + op);
}

inline Value Interpreter::evalExpr(Expr* e, EnvPtr env) {
    switch (e->kind) {
        case Expr::IntLit:  return Value(static_cast<IntExpr*>(e)->v);
        case Expr::FltLit:  return Value(static_cast<FltExpr*>(e)->v);
        case Expr::ChrLit:  return Value(static_cast<ChrExpr*>(e)->v);
        case Expr::StrLit:  return Value(static_cast<StrExpr*>(e)->v);
        case Expr::BolLit:  return Value(static_cast<BolExpr*>(e)->v);
        case Expr::NilLit:  return Value{};

        case Expr::Ident: {
            auto* id = static_cast<IdentExpr*>(e);
            if (env->isMoved(id->name))
                throw ArcError::at(e->line, e->col,
                    "use-after-move: variable '" + id->name + "' was moved");
            auto* slot = env->find(id->name);
            if (slot) return *slot;
            auto it = prog_->functions.find(id->name);
            if (it != prog_->functions.end()) {
                EnvPtr cenv = global_;
                if (!it->second->ownerModule.empty()) {
                    auto mit = moduleEnvs_.find(it->second->ownerModule);
                    if (mit != moduleEnvs_.end()) cenv = mit->second;
                }
                auto cl = std::make_shared<Closure>(it->second, cenv);
                return Value(cl);
            }
            throw ArcError::at(e->line, e->col,
                "undefined variable: " + id->name);
        }

        case Expr::Binary:
            return evalBinaryExpr(static_cast<BinaryExpr*>(e), env);
        case Expr::Unary: {
            auto* u = static_cast<UnaryExpr*>(e);
            auto v = evalExpr(u->operand.get(), env);
            return evalUnary(u->op, v);
        }
        case Expr::NamedInstr:
            return evalNamedInstr(static_cast<NamedInstrExpr*>(e), env);

        case Expr::Call:
            return evalCall(static_cast<CallExpr*>(e), env);
        case Expr::MethodCall:
            return evalMethodCall(static_cast<MethodCallExpr*>(e), env);
        case Expr::CalCall:
            return evalCalCall(static_cast<CalCallExpr*>(e), env);

        case Expr::Index:
            return evalIndex(static_cast<IndexExpr*>(e), env);
        case Expr::Slice:
            return evalSlice(static_cast<SliceExpr*>(e), env);
        case Expr::Member:
            return evalMember(static_cast<MemberExpr*>(e), env);
        case Expr::TupleIndex:
            return evalTupleIndex(static_cast<TupleIndexExpr*>(e), env);

        case Expr::OptHas:
            return evalOptHas(static_cast<OptHasExpr*>(e), env);
        case Expr::OptVal:
            return evalOptVal(static_cast<OptValExpr*>(e), env);

        case Expr::AsConv:
            return evalAsConv(static_cast<AsConvExpr*>(e), env);

        case Expr::OkExpr:  return evalOk(static_cast<OkExpr*>(e), env);
        case Expr::ErExpr:  return evalEr(static_cast<ErExpr*>(e), env);
        case Expr::TryExpr: return evalTry(static_cast<TryExpr*>(e), env);
        case Expr::UnwExpr: return evalUnw(static_cast<UnwExpr*>(e), env);
        case Expr::OrExpr:  return evalOr(static_cast<OrExpr*>(e), env);

        case Expr::ArrayLit:
            return evalArrayLit(static_cast<ArrayLitExpr*>(e), env);
        case Expr::TupleLit:
            return evalTupleLit(static_cast<TupleLitExpr*>(e), env);
        case Expr::RecLit:
            return evalRecLit(static_cast<RecLitExpr*>(e), env);
        case Expr::ClsLit:
            return evalClsLit(static_cast<ClsLitExpr*>(e), env);
        case Expr::EnmLit:
            return evalEnmLit(static_cast<EnmLitExpr*>(e), env);
        case Expr::RangeLit:
            return evalRangeLit(static_cast<RangeLitExpr*>(e), env);

        case Expr::SizeofExpr:
            return evalSizeof(static_cast<SizeofExpr*>(e), env);

        case Expr::SelExpr: return evalSel(static_cast<SelExpr*>(e), env);
        case Expr::BlkExpr: return evalBlk(static_cast<BlkExpr*>(e), env);
        case Expr::LamExpr: return evalLambda(static_cast<LamExpr*>(e), env);
        case Expr::FmtExpr: return evalFmt(static_cast<FmtExpr*>(e), env);

        case Expr::PipeExpr: return evalPipe(static_cast<PipeExpr*>(e), env);
        case Expr::RseExpr:  return evalRse(static_cast<RseExpr*>(e), env);
        case Expr::CteCall:
            throw ArcError::at(e->line, e->col,
                "cte: expression can only be used at compile time");

        case Expr::AddrExpr: {
            auto* ae = static_cast<AddrExpr*>(e);
            Value* slot = nullptr;
            if (auto* id = dynamic_cast<IdentExpr*>(ae->target.get())) {
                slot = env->find(id->name);
                if (!slot)
                    throw ArcError::at(e->line, e->col,
                        "cannot take address of undefined variable: " + id->name);
            } else if (auto* m = dynamic_cast<MemberExpr*>(ae->target.get())) {
                auto recv = evalExpr(m->receiver.get(), env);
                if (!isObj(recv))
                    throw ArcError::at(e->line, e->col,
                        "& .field requires object receiver");
                auto obj = std::get<ObjectPtr>(recv.v);
                auto it = obj->fields.find(m->name);
                if (it == obj->fields.end())
                    throw ArcError::at(e->line, e->col,
                        "no field '" + m->name + "' to take address of");
                slot = &it->second;
            } else {
                throw ArcError::at(e->line, e->col,
                    "& only supports identifiers and object fields");
            }
            auto pv = std::make_shared<PtrValue>();
            pv->target = slot;
            pv->owned  = false;
            return Value(pv);
        }

        case Expr::DerefExpr: {
            auto* de = static_cast<DerefExpr*>(e);
            auto pv = evalExpr(de->pointer.get(), env);
            if (!isPtr(pv))
                throw ArcError::at(e->line, e->col,
                    "'*' / '^' requires a pointer operand");
            auto p = std::get<PtrPtr>(pv.v);
            if (!p || !p->target)
                throw ArcError::at(e->line, e->col, "null pointer dereference");
            return *p->target;
        }
    }
    throw ArcError::at(e->line, e->col, "unknown expression kind");
}

inline Value Interpreter::evalUnary(const std::string& op, const Value& v) {
    if (op == "-") {
        if (isInt(v)) return Value(-std::get<int64_t>(v.v));
        if (isFlt(v)) return Value(-std::get<double>(v.v));
        if (isChr(v)) return Value(static_cast<int64_t>(-asChr(v)));
        if (isStr(v)) {
            std::string s = asStr(v);
            std::reverse(s.begin(), s.end());
            return Value(s);
        }
        throw ArcError("unary '-' requires number or string");
    }
    if (op == "!") {
        if (!isBol(v)) throw ArcError("'!' requires bol");
        return Value(!std::get<bool>(v.v));
    }
    if (op == "~") {
        if (!isIntegral(v)) throw ArcError("'~' requires integer");
        return Value(~asInt(v));
    }
    if (op == "#") {
        if (isArr(v))
            return Value(static_cast<int64_t>(
                std::get<ArrayPtr>(v.v)->elems.size()));
        if (isSlice(v))
            return Value(std::get<SlicePtr>(v.v)->length);
        if (isStr(v))
            return Value(static_cast<int64_t>(asStr(v).size()));
        if (isMap(v))
            return Value(static_cast<int64_t>(
                std::get<MapPtr>(v.v)->data.size()));
        throw ArcError("'#' requires array, slice, string, or map");
    }
    throw ArcError("unknown unary op: " + op);
}

inline Value Interpreter::evalBinaryExpr(BinaryExpr* b, EnvPtr env) {
    if (b->op == "&&") {
        auto l = evalExpr(b->lhs.get(), env);
        if (!isBol(l))
            throw ArcError::at(b->line, b->col, "'&&' requires bol");
        if (!std::get<bool>(l.v)) return Value(false);
        auto r = evalExpr(b->rhs.get(), env);
        if (!isBol(r))
            throw ArcError::at(b->line, b->col, "'&&' requires bol");
        return Value(std::get<bool>(r.v));
    }
    if (b->op == "||") {
        auto l = evalExpr(b->lhs.get(), env);
        if (!isBol(l))
            throw ArcError::at(b->line, b->col, "'||' requires bol");
        if (std::get<bool>(l.v)) return Value(true);
        auto r = evalExpr(b->rhs.get(), env);
        if (!isBol(r))
            throw ArcError::at(b->line, b->col, "'||' requires bol");
        return Value(std::get<bool>(r.v));
    }
    auto l = evalExpr(b->lhs.get(), env);
    auto r = evalExpr(b->rhs.get(), env);

    if (isObj(l)) {
        auto obj = std::get<ObjectPtr>(l.v);
        if (obj) {
            static const std::unordered_map<std::string, std::string> opTrait = {
                {"+","Add"},{"-","Sub"},{"*","Mul"},{"/","Div"},{"%","Mod"},
                {"==","Equ"},{"!=","Neq"},{"<","Lss"},{">","Grt"},
                {"<=","Leq"},{">=","Geq"}
            };
            auto tnameIt = opTrait.find(b->op);
            if (tnameIt != opTrait.end()) {
                auto tit = prog_->traitsByType.find(obj->className);
                if (tit != prog_->traitsByType.end()) {
                    for (auto& tra : tit->second) {
                        if (tra && tra->name == tnameIt->second &&
                            !tra->methods.empty()) {
                            std::vector<Value> args;
                            args.push_back(l);
                            args.push_back(r);
                            return callFunction(tra->methods[0],
                                                std::move(args), env);
                        }
                    }
                }
            }
        }
    }

    return applyBinary(b->op, l, r);
}

inline Value Interpreter::evalNamedInstr(NamedInstrExpr* e, EnvPtr env) {
    const std::string& n = e->name;
    std::vector<Value> args;
    args.reserve(e->args.size());
    for (auto& a : e->args) args.push_back(evalExpr(a.get(), env));

    auto need = [&](size_t k) {
        if (args.size() != k)
            throw ArcError::at(e->line, e->col,
                n + " expects " + std::to_string(k) + " argument(s)");
    };

    if (n == "add") { need(2); return applyBinary("+", args[0], args[1]); }
    if (n == "sub") { need(2); return applyBinary("-", args[0], args[1]); }
    if (n == "mul") { need(2); return applyBinary("*", args[0], args[1]); }
    if (n == "div") { need(2); return applyBinary("/", args[0], args[1]); }
    if (n == "mod") { need(2); return applyBinary("%", args[0], args[1]); }
    if (n == "pow") { need(2); return applyBinary("^", args[0], args[1]); }
    if (n == "neg") { need(1); return evalUnary("-", args[0]); }
    if (n == "abs") {
        need(1);
        if (isInt(args[0])) {
            auto v = std::get<int64_t>(args[0].v);
            return Value(v < 0 ? -v : v);
        }
        if (isFlt(args[0])) {
            auto v = std::get<double>(args[0].v);
            return Value(v < 0 ? -v : v);
        }
        if (isChr(args[0])) {
            return Value(static_cast<int64_t>(asChr(args[0])));
        }
        throw ArcError::at(e->line, e->col, "abs requires numeric");
    }
    if (n == "min") {
        need(2);
        return truthy(applyBinary("<", args[0], args[1])) ? args[0] : args[1];
    }
    if (n == "max") {
        need(2);
        return truthy(applyBinary(">", args[0], args[1])) ? args[0] : args[1];
    }
    if (n == "shl") { need(2); return applyBinary("<<", args[0], args[1]); }
    if (n == "shr") { need(2); return applyBinary(">>", args[0], args[1]); }
    if (n == "lsr") { need(2); return applyBinary(">>>", args[0], args[1]); }
    if (n == "and") { need(2); return applyBinary("&", args[0], args[1]); }
    if (n == "or")  { need(2); return applyBinary("|", args[0], args[1]); }
    if (n == "xor") {
        need(2);
        if (isIntegral(args[0]) && isIntegral(args[1]))
            return Value(asInt(args[0]) ^ asInt(args[1]));
        if (isBol(args[0]) && isBol(args[1]))
            return Value(std::get<bool>(args[0].v) !=
                         std::get<bool>(args[1].v));
        throw ArcError::at(e->line, e->col, "xor requires ints or bools");
    }
    if (n == "not") {
        need(1);
        if (isBol(args[0])) return Value(!std::get<bool>(args[0].v));
        if (isIntegral(args[0])) return Value(~asInt(args[0]));
        throw ArcError::at(e->line, e->col, "not requires int or bool");
    }
    if (n == "equ") { need(2); return Value(valueEquals(args[0], args[1])); }
    if (n == "neq") { need(2); return Value(!valueEquals(args[0], args[1])); }
    if (n == "grt") { need(2); return applyBinary(">",  args[0], args[1]); }
    if (n == "lss") { need(2); return applyBinary("<",  args[0], args[1]); }
    if (n == "geq") { need(2); return applyBinary(">=", args[0], args[1]); }
    if (n == "leq") { need(2); return applyBinary("<=", args[0], args[1]); }
    if (n == "sel") {
        need(3);
        return truthy(args[0]) ? args[1] : args[2];
    }
    throw ArcError::at(e->line, e->col, "unknown named instruction: " + n);
}

inline Value Interpreter::evalCall(CallExpr* e, EnvPtr env) {
    auto callee = evalExpr(e->callee.get(), env);
    std::vector<Value> args;
    args.reserve(e->args.size());
    for (auto& a : e->args) args.push_back(evalExpr(a.get(), env));

    if (!isClosure(callee))
        throw ArcError::at(e->line, e->col, "call target is not a function");

    auto cl = std::get<ClosurePtr>(callee.v);
    if (cl->fn->isCoroutine)
        return startCoroutine(cl->fn, std::move(args), cl->env);
    return callFunction(cl->fn, std::move(args), cl->env);
}

inline Value Interpreter::evalCalCall(CalCallExpr* e, EnvPtr env) {
    auto target = evalExpr(e->target.get(), env);
    std::vector<Value> args;
    args.reserve(e->args.size());
    for (auto& a : e->args) args.push_back(evalExpr(a.get(), env));

    if (!isClosure(target))
        throw ArcError::at(e->line, e->col, "cal target is not callable");

    auto cl = std::get<ClosurePtr>(target.v);
    if (cl->fn->isCoroutine)
        return startCoroutine(cl->fn, std::move(args), cl->env);
    return callFunction(cl->fn, std::move(args), cl->env);
}

inline FunctionPtr Interpreter::lookupMethod(const std::string& className,
                                              const std::string& method) {
    std::string cur = className;
    while (!cur.empty()) {
        auto cit = prog_->classes.find(cur);
        if (cit == prog_->classes.end()) break;
        for (auto& m : cit->second->members) {
            bool isMethod = (m.kind == ClassMember::Method ||
                             m.kind == ClassMember::VirtualMethod ||
                             m.kind == ClassMember::FinalMethod ||
                             m.kind == ClassMember::StaticMethod ||
                             m.kind == ClassMember::Dtor);
            if (isMethod && m.method && m.method->name == method)
                return m.method;
        }
        cur = cit->second->superClass;
    }

    auto tit = prog_->traitsByType.find(className);
    if (tit != prog_->traitsByType.end()) {
        for (auto& tra : tit->second) {
            if (!tra) continue;
            for (auto& fn : tra->methods) {
                if (fn && fn->name == method) return fn;
            }
        }
    }

    return nullptr;
}

inline Value Interpreter::evalMethodCall(MethodCallExpr* e, EnvPtr env) {
    if (auto* id = dynamic_cast<IdentExpr*>(e->receiver.get())) {
        auto* slot = env->find(id->name);
        if (!slot && prog_->classes.count(id->name)) {
            std::vector<Value> args;
            args.reserve(e->args.size());
            for (auto& a : e->args) args.push_back(evalExpr(a.get(), env));
            auto fn = lookupMethod(id->name, e->method);
            if (!fn || !fn->isStatic)
                throw ArcError::at(e->line, e->col,
                    "class '" + id->name +
                    "' has no static method '" + e->method + "'");
            return callFunction(fn, std::move(args), env);
        }
    }

    auto recv = evalExpr(e->receiver.get(), env);
    std::vector<Value> args;
    args.reserve(e->args.size());
    for (auto& a : e->args) args.push_back(evalExpr(a.get(), env));

    if (isStr(recv)) {
        const std::string& s = asStr(recv);
        if (e->method == "len")
            return Value(static_cast<int64_t>(s.size()));
        if (e->method == "cat") {
            if (args.empty() || !isStr(args[0]))
                throw ArcError::at(e->line, e->col,
                    "str.cat requires a string argument");
            return Value(s + asStr(args[0]));
        }
        if (e->method == "sub") {
            if (args.size() != 2)
                throw ArcError::at(e->line, e->col, "str.sub requires 2 args");
            int64_t a = asInt(args[0]), b = asInt(args[1]);
            if (a < 0 || b > (int64_t)s.size() || a > b)
                throw ArcError::at(e->line, e->col, "str.sub out of range");
            return Value(s.substr(static_cast<size_t>(a),
                                   static_cast<size_t>(b - a)));
        }
        if (e->method == "upper" || e->method == "lower") {
            std::string out = s;
            for (auto& c : out) {
                c = (e->method == "upper")
                    ? static_cast<char>(std::toupper(
                          static_cast<unsigned char>(c)))
                    : static_cast<char>(std::tolower(
                          static_cast<unsigned char>(c)));
            }
            return Value(out);
        }
        throw ArcError::at(e->line, e->col,
            "no method '" + e->method + "' on str");
    }

    if (isArr(recv)) {
        auto arr = std::get<ArrayPtr>(recv.v);
        if (e->method == "len")
            return Value(static_cast<int64_t>(arr->elems.size()));
        if (e->method == "push") {
            if (args.empty())
                throw ArcError::at(e->line, e->col, "arr.push requires arg");
            arr->elems.push_back(args[0]);
            return Value{};
        }
        if (e->method == "pop") {
            if (arr->elems.empty())
                throw ArcError::at(e->line, e->col, "pop from empty array");
            auto v = arr->elems.back();
            arr->elems.pop_back();
            return v;
        }
        if (e->method == "get") {
            if (args.empty())
                throw ArcError::at(e->line, e->col, "arr.get requires arg");
            int64_t i = asInt(args[0]);
            if (i < 0 || i >= (int64_t)arr->elems.size())
                throw ArcError::at(e->line, e->col, "array index out of range");
            return arr->elems[static_cast<size_t>(i)];
        }
        throw ArcError::at(e->line, e->col,
            "no method '" + e->method + "' on arr");
    }

    if (isSlice(recv)) {
        auto sl = std::get<SlicePtr>(recv.v);
        if (e->method == "len")
            return Value(sl->length);
        if (e->method == "get") {
            if (args.empty())
                throw ArcError::at(e->line, e->col, "slc.get requires arg");
            int64_t i = asInt(args[0]);
            if (i < 0 || i >= sl->length)
                throw ArcError::at(e->line, e->col, "slice index out of range");
            return sl->source->elems[sl->offset + i];
        }
        throw ArcError::at(e->line, e->col,
            "no method '" + e->method + "' on slc");
    }

    if (isMap(recv)) {
        auto m = std::get<MapPtr>(recv.v);
        if (e->method == "has") {
            if (args.empty())
                throw ArcError::at(e->line, e->col, "map.has requires arg");
            auto k = valueToMapKeyAt(args[0], e->line, e->col);
            return Value(m->data.count(k) > 0);
        }
        if (e->method == "del") {
            if (args.empty())
                throw ArcError::at(e->line, e->col, "map.del requires arg");
            auto k = valueToMapKeyAt(args[0], e->line, e->col);
            m->data.erase(k);
            return Value{};
        }
        if (e->method == "len")
            return Value(static_cast<int64_t>(m->data.size()));
        throw ArcError::at(e->line, e->col,
            "no method '" + e->method + "' on map");
    }

    if (isObj(recv)) {
        auto obj = std::get<ObjectPtr>(recv.v);
        auto fn = lookupMethod(obj->className, e->method);
        if (fn) {
            std::vector<Value> full;
            if (!fn->isStatic) full.push_back(recv);
            for (auto& a : args) full.push_back(a);
            return callFunction(fn, std::move(full), env);
        }
        throw ArcError::at(e->line, e->col,
            "no method '" + e->method + "' in class " + obj->className);
    }

    throw ArcError::at(e->line, e->col,
        "method '" + e->method + "' not supported on this value");
}

inline Value Interpreter::evalIndex(IndexExpr* e, EnvPtr env) {
    auto cont = evalExpr(e->container.get(), env);
    auto idx  = evalExpr(e->index.get(), env);

    if (isArr(cont)) {
        auto arr = std::get<ArrayPtr>(cont.v);
        int64_t i = asInt(idx);
        if (i < 0 || i >= (int64_t)arr->elems.size())
            throw ArcError::at(e->line, e->col, "array index out of range");
        return arr->elems[static_cast<size_t>(i)];
    }
    if (isStr(cont)) {
        const std::string& s = asStr(cont);
        int64_t i = asInt(idx);
        if (i < 0 || i >= (int64_t)s.size())
            throw ArcError::at(e->line, e->col, "string index out of range");
        return Value(s[static_cast<size_t>(i)]);
    }
    if (isSlice(cont)) {
        auto sl = std::get<SlicePtr>(cont.v);
        int64_t i = asInt(idx);
        if (i < 0 || i >= sl->length)
            throw ArcError::at(e->line, e->col, "slice index out of range");
        return sl->source->elems[sl->offset + i];
    }
    if (isMap(cont)) {
        auto m = std::get<MapPtr>(cont.v);
        auto k = valueToMapKeyAt(idx, e->line, e->col);
        auto it = m->data.find(k);
        if (it == m->data.end()) return Value{};
        return it->second;
    }
    throw ArcError::at(e->line, e->col, "indexing not supported on this value");
}

inline Value Interpreter::evalSlice(SliceExpr* e, EnvPtr env) {
    auto cont = evalExpr(e->container.get(), env);

    if (isSlice(cont)) {
        auto sl = std::get<SlicePtr>(cont.v);
        int64_t from = e->from ? asInt(evalExpr(e->from.get(), env)) : 0;
        int64_t to   = e->to   ? asInt(evalExpr(e->to.get(), env))   : sl->length;
        if (from < 0 || to > sl->length || from > to)
            throw ArcError::at(e->line, e->col, "slice out of range");
        auto out = std::make_shared<SliceValue>();
        out->source = sl->source;
        out->offset = sl->offset + from;
        out->length = to - from;
        return Value(out);
    }

    if (isArr(cont)) {
        auto arr = std::get<ArrayPtr>(cont.v);
        int64_t from = e->from ? asInt(evalExpr(e->from.get(), env)) : 0;
        int64_t n    = (int64_t)arr->elems.size();
        int64_t to   = e->to   ? asInt(evalExpr(e->to.get(), env))   : n;
        if (from < 0 || to > n || from > to)
            throw ArcError::at(e->line, e->col, "slice out of range");
        auto out = std::make_shared<SliceValue>();
        out->source = arr;
        out->offset = from;
        out->length = to - from;
        return Value(out);
    }

    if (isStr(cont)) {
        const std::string& s = asStr(cont);
        int64_t from = e->from ? asInt(evalExpr(e->from.get(), env)) : 0;
        int64_t n    = (int64_t)s.size();
        int64_t to   = e->to   ? asInt(evalExpr(e->to.get(), env))   : n;
        if (from < 0 || to > n || from > to)
            throw ArcError::at(e->line, e->col, "slice out of range");
        return Value(s.substr(static_cast<size_t>(from),
                              static_cast<size_t>(to - from)));
    }

    throw ArcError::at(e->line, e->col, "slice requires array or string");
}

inline Value Interpreter::evalMember(MemberExpr* e, EnvPtr env) {
    auto recv = evalExpr(e->receiver.get(), env);
    if (isObj(recv)) {
        auto obj = std::get<ObjectPtr>(recv.v);
        auto it = obj->fields.find(e->name);
        if (it == obj->fields.end())
            throw ArcError::at(e->line, e->col,
                "no field '" + e->name + "' in " + obj->className);
        return it->second;
    }
    throw ArcError::at(e->line, e->col, "member access on non-object");
}

inline Value Interpreter::evalTupleIndex(TupleIndexExpr* e, EnvPtr env) {
    auto recv = evalExpr(e->receiver.get(), env);
    if (isArr(recv)) {
        auto arr = std::get<ArrayPtr>(recv.v);
        int i = e->index;
        if (i < 0 || i >= (int64_t)arr->elems.size())
            throw ArcError::at(e->line, e->col, "tuple index out of range");
        return arr->elems[static_cast<size_t>(i)];
    }
    throw ArcError::at(e->line, e->col, "tuple index on non-tuple");
}

inline Value Interpreter::evalOptHas(OptHasExpr* e, EnvPtr env) {
    auto v = evalExpr(e->receiver.get(), env);
    if (isOpt(v)) {
        auto o = std::get<OptPtr>(v.v);
        return Value(o && o->has);
    }
    throw ArcError::at(e->line, e->col, ".has on non-opt value");
}

inline Value Interpreter::evalOptVal(OptValExpr* e, EnvPtr env) {
    auto v = evalExpr(e->receiver.get(), env);
    if (isOpt(v)) {
        auto o = std::get<OptPtr>(v.v);
        if (!o || !o->has)
            throw ArcError::at(e->line, e->col, ".val on empty opt");
        return o->value;
    }
    throw ArcError::at(e->line, e->col, ".val on non-opt value");
}

inline Value Interpreter::evalAsConv(AsConvExpr* e, EnvPtr env) {
    auto v = evalExpr(e->value.get(), env);
    auto& target = e->target;
    if (!target) return Value{};

    auto ok = [](Value x) {
        return Value(std::make_shared<OptValue>(true, std::move(x)));
    };
    auto none = []() {
        return Value(std::make_shared<OptValue>(false, Value{}));
    };

    if (target->kind == Type::Int) {
        if (isInt(v)) return ok(v);
        if (isFlt(v)) return ok(Value(static_cast<int64_t>(std::get<double>(v.v))));
        if (isChr(v)) return ok(Value(static_cast<int64_t>(asChr(v))));
        if (isBol(v)) return ok(Value(std::get<bool>(v.v) ? int64_t(1) : int64_t(0)));
        if (isStr(v)) {
            try { return ok(Value(static_cast<int64_t>(std::stoll(asStr(v))))); }
            catch (...) { return none(); }
        }
        return none();
    }
    if (target->kind == Type::Flt) {
        if (isFlt(v)) return ok(v);
        if (isInt(v)) return ok(Value(static_cast<double>(std::get<int64_t>(v.v))));
        if (isChr(v)) return ok(Value(static_cast<double>(asChr(v))));
        if (isBol(v)) return ok(Value(std::get<bool>(v.v) ? 1.0 : 0.0));
        if (isStr(v)) {
            try { return ok(Value(std::stod(asStr(v)))); }
            catch (...) { return none(); }
        }
        return none();
    }
    if (target->kind == Type::Chr) {
        if (isChr(v)) return ok(v);
        if (isInt(v)) {
            int64_t x = std::get<int64_t>(v.v);
            if (x < 0 || x > 0xFF) return none();
            return ok(Value(static_cast<char>(static_cast<uint8_t>(x))));
        }
        return none();
    }
    if (target->kind == Type::Bol) {
        if (isBol(v)) return ok(v);
        return ok(Value(truthy(v)));
    }
    if (target->kind == Type::Str) {
        return ok(Value(valueToString(v)));
    }

    return ok(v);
}

inline Value Interpreter::evalOk(OkExpr* e, EnvPtr env) {
    return Value(std::make_shared<RltValue>(true,
                    evalExpr(e->value.get(), env)));
}

inline Value Interpreter::evalEr(ErExpr* e, EnvPtr env) {
    return Value(std::make_shared<RltValue>(false,
                    evalExpr(e->value.get(), env)));
}

inline Value Interpreter::evalTry(TryExpr* e, EnvPtr env) {
    auto v = evalExpr(e->value.get(), env);

    if (isRlt(v)) {
        auto r = std::get<RltPtr>(v.v);
        if (!r->isOk) throw TryReturnSignal{r->value};
        return r->value;
    }
    if (isOpt(v)) {
        auto o = std::get<OptPtr>(v.v);
        if (!o || !o->has) throw TryReturnSignal{Value{}};
        return o->value;
    }
    return v;
}

inline Value Interpreter::evalUnw(UnwExpr* e, EnvPtr env) {
    auto v = evalExpr(e->value.get(), env);

    if (isNil(v)) return evalExpr(e->fallback.get(), env);

    if (isOpt(v)) {
        auto o = std::get<OptPtr>(v.v);
        if (!o || !o->has) return evalExpr(e->fallback.get(), env);
        return o->value;
    }
    if (isRlt(v)) {
        auto r = std::get<RltPtr>(v.v);
        if (!r->isOk) return evalExpr(e->fallback.get(), env);
        return r->value;
    }
    return v;
}

inline Value Interpreter::evalOr(OrExpr* e, EnvPtr env) {
    auto v = evalExpr(e->lhs.get(), env);

    if (isNil(v)) return evalExpr(e->rhs.get(), env);

    if (isOpt(v)) {
        auto o = std::get<OptPtr>(v.v);
        if (!o || !o->has) return evalExpr(e->rhs.get(), env);
        return o->value;
    }
    if (isRlt(v)) {
        auto r = std::get<RltPtr>(v.v);
        if (!r->isOk) return evalExpr(e->rhs.get(), env);
        return r->value;
    }
    return v;
}

inline Value Interpreter::evalArrayLit(ArrayLitExpr* e, EnvPtr env) {
    std::vector<Value> elems;
    elems.reserve(e->elems.size());
    for (auto& x : e->elems) elems.push_back(evalExpr(x.get(), env));
    return Value(std::make_shared<Array>(std::move(elems)));
}

inline Value Interpreter::evalTupleLit(TupleLitExpr* e, EnvPtr env) {
    std::vector<Value> elems;
    elems.reserve(e->elems.size());
    for (auto& x : e->elems) elems.push_back(evalExpr(x.get(), env));
    return Value(std::make_shared<Array>(std::move(elems)));
}

inline Value Interpreter::evalRecLit(RecLitExpr* e, EnvPtr env) {
    auto obj = std::make_shared<Object>(e->typeName);

    auto it = prog_->records.find(e->typeName);
    std::vector<std::string> fieldOrder;
    if (it != prog_->records.end()) {
        for (auto& f : it->second->fields) {
            obj->fields[f.name] = defaultOfType(f.type);
            fieldOrder.push_back(f.name);
        }
    }

    if (!e->positionalArgs.empty() && !e->fields.empty()) {
        throw ArcError::at(e->line, e->col,
            "record '" + e->typeName +
            "': cannot mix positional args and .field = v");
    }

    if (!e->positionalArgs.empty()) {
        if (it == prog_->records.end())
            throw ArcError::at(e->line, e->col,
                "unknown record: " + e->typeName);
        for (size_t i = 0; i < e->positionalArgs.size(); ++i) {
            if (i >= fieldOrder.size())
                throw ArcError::at(e->line, e->col,
                    "too many positional args for record " + e->typeName);
            obj->fields[fieldOrder[i]] =
                evalExpr(e->positionalArgs[i].get(), env);
        }
    } else {
        for (auto& fi : e->fields)
            obj->fields[fi.name] = evalExpr(fi.value.get(), env);
    }

    return Value(obj);
}

inline Value Interpreter::evalClsLit(ClsLitExpr* e, EnvPtr env) {
    auto obj = std::make_shared<Object>(e->typeName);
    auto it = prog_->classes.find(e->typeName);

    {
        std::string cur = e->typeName;
        std::vector<std::string> chain;
        while (!cur.empty()) {
            chain.push_back(cur);
            auto cit = prog_->classes.find(cur);
            if (cit == prog_->classes.end()) break;
            cur = cit->second->superClass;
        }
        for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
            auto cit = prog_->classes.find(*rit);
            if (cit == prog_->classes.end()) continue;
            for (auto& m : cit->second->members) {
                if (m.kind == ClassMember::Field)
                    obj->fields[m.field.name] = defaultOfType(m.field.type);
            }
        }
    }

    Value objVal(obj);

    FunctionPtr anyCtor;
    FunctionPtr ctor;
    if (it != prog_->classes.end()) {
        for (auto& m : it->second->members) {
            if (m.kind != ClassMember::Ctor || !m.method) continue;
            if (!anyCtor) anyCtor = m.method;
            if (m.method->params.size() < 1) continue;
            if (m.method->params.size() - 1 == e->positionalArgs.size()) {
                ctor = m.method;
                break;
            }
        }
    }

    std::vector<Value> posArgs;
    posArgs.reserve(e->positionalArgs.size());
    for (auto& a : e->positionalArgs)
        posArgs.push_back(evalExpr(a.get(), env));

    if (anyCtor) {
        if (!e->fields.empty())
            throw ArcError::at(e->line, e->col,
                "class '" + e->typeName +
                "' has ini; use positional args, not .field = v");
        if (!ctor)
            throw ArcError::at(e->line, e->col,
                "class '" + e->typeName + "' has no ini matching " +
                std::to_string(e->positionalArgs.size()) + " argument(s)");

        std::vector<Value> args;
        args.push_back(objVal);
        for (auto& a : posArgs) args.push_back(a);
        callFunction(ctor, std::move(args), env);
    } else {
        if (!posArgs.empty()) {
            if (!e->fields.empty())
                throw ArcError::at(e->line, e->col,
                    "cannot mix positional args and .field = v");
            if (it == prog_->classes.end())
                throw ArcError::at(e->line, e->col,
                    "unknown class: " + e->typeName);
            size_t idx = 0;
            for (auto& m : it->second->members) {
                if (m.kind != ClassMember::Field) continue;
                if (idx >= posArgs.size()) break;
                obj->fields[m.field.name] = posArgs[idx++];
            }
            if (idx < posArgs.size())
                throw ArcError::at(e->line, e->col,
                    "too many positional args for class " + e->typeName);
        } else {
            for (auto& fi : e->fields)
                obj->fields[fi.name] = evalExpr(fi.value.get(), env);
        }
    }

    return objVal;
}

inline Value Interpreter::evalEnmLit(EnmLitExpr* e, EnvPtr env) {
    auto obj = std::make_shared<Object>(e->typeName + "." + e->variant);
    if (!e->args.empty()) {
        std::vector<Value> vs;
        vs.reserve(e->args.size());
        for (auto& a : e->args) vs.push_back(evalExpr(a.get(), env));
        obj->fields["payload"] = Value(std::make_shared<Array>(std::move(vs)));
    }
    return Value(obj);
}

inline Value Interpreter::evalRangeLit(RangeLitExpr* e, EnvPtr env) {
    auto r = std::make_shared<RangeValue>();
    r->from = evalExpr(e->from.get(), env);
    r->to   = evalExpr(e->to.get(), env);
    r->exclusive = e->exclusive;
    if (e->step) {
        r->hasStep = true;
        r->step    = evalExpr(e->step.get(), env);
    }
    return Value(r);
}

inline Value Interpreter::evalSizeof(SizeofExpr* e, EnvPtr) {
    if (!e->operand) return Value(int64_t(0));
    return Value(e->isAlign ? e->operand->align() : e->operand->size());
}

inline Value Interpreter::evalSel(SelExpr* e, EnvPtr env) {
    auto c = evalExpr(e->cond.get(), env);
    return truthy(c) ? evalExpr(e->thenV.get(), env)
                     : evalExpr(e->elseV.get(), env);
}

inline Value Interpreter::evalBlk(BlkExpr* e, EnvPtr env) {
    auto local = std::make_shared<Env>(env);
    local->vars["res"] = Value{};
    ScopeGuard _sg(this, local);
    try {
        execBlock(e->body, local);
    } catch (ReturnSignal& r) {
        return r.value;
    }
    auto it = local->vars.find("res");
    if (it != local->vars.end()) return it->second;
    return Value{};
}

inline Value Interpreter::evalLambda(LamExpr* e, EnvPtr env) {
    auto fn = std::make_shared<Function>();
    fn->name = "<lambda>";
    for (auto& p : e->params) {
        Param param;
        param.name = p.first;
        param.type = p.second;
        fn->params.push_back(std::move(param));
    }
    fn->retType = e->retType;

    if (e->single) {
        auto r = std::make_unique<RetStmt>();
        r->line  = e->line;
        r->value = std::move(e->single);
        fn->body.push_back(std::move(r));
    } else {
        fn->body = std::move(e->body);
    }
    fn->hasBody = true;

    auto lambdaEnv = std::make_shared<Env>(global_);
    for (auto& cap : e->captures) {
        auto* slot = env->find(cap.outerName);
        if (!slot)
            throw ArcError::at(e->line, e->col,
                "captured variable not found: " + cap.outerName);
        lambdaEnv->vars[cap.innerName] = *slot;
    }
    auto cl = std::make_shared<Closure>(fn, lambdaEnv);
    return Value(cl);
}

inline Value Interpreter::evalFmt(FmtExpr* e, EnvPtr env) {
    std::string out;
    for (auto& p : e->parts) {
        if (p.isExpr) out += valueToString(evalExpr(p.expr.get(), env));
        else          out += p.text;
    }
    return Value(out);
}

inline Value Interpreter::evalPipe(PipeExpr* e, EnvPtr env) {
    auto it = prog_->functions.find(e->fnName);
    if (it == prog_->functions.end())
        throw ArcError::at(e->line, e->col,
            "pipeline target not found: " + e->fnName);
    std::vector<Value> args;
    args.reserve(e->args.size() + 1);
    args.push_back(evalExpr(e->left.get(), env));
    for (auto& a : e->args) args.push_back(evalExpr(a.get(), env));
    return callFunction(it->second, std::move(args), env);
}

inline Value Interpreter::evalRse(RseExpr* e, EnvPtr env) {
    auto h = evalExpr(e->handle.get(), env);
    if (!isCoroutine(h))
        throw ArcError::at(e->line, e->col, "rse requires a coroutine handle");
    auto co = std::get<CoroutinePtr>(h.v);

    if (co->state == Coroutine::Done)
        return Value(std::make_shared<OptValue>(false, Value{}));

    Value v = resumeCoroutine(co, Value{});

    if (co->state == Coroutine::Done)
        return Value(std::make_shared<OptValue>(false, Value{}));

    return Value(std::make_shared<OptValue>(true, v));
}

// P9 结束。
// ============================================================================
// P10  Interpreter —— 语句执行 + 协程状态机
// ============================================================================

struct JumpSignal { std::string label; };

struct SuspendSignal {};

inline void Interpreter::execSuite(const std::vector<StmtPtr>& body, EnvPtr env) {
    std::unordered_map<std::string, size_t> labels;
    for (size_t i = 0; i < body.size(); ++i) {
        if (auto* lbl = dynamic_cast<LabelStmt*>(body[i].get()))
            labels[lbl->name] = i;
    }

    size_t pc = 0;
    while (pc < body.size()) {
        bool jumped = false;
        try {
            execStmt(body[pc].get(), env);
        } catch (JumpSignal& j) {
            auto it = labels.find(j.label);
            if (it == labels.end()) throw;
            pc = it->second;
            jumped = true;
        }
        if (!jumped) ++pc;
    }
}

inline void Interpreter::execBlock(const std::vector<StmtPtr>& body, EnvPtr env) {
    execSuite(body, env);
}

inline bool labelMatches(const std::string& signalLabel,
                         const std::string& loopLabel) {
    if (signalLabel.empty()) return true;
    return signalLabel == loopLabel;
}

inline void Interpreter::execStmt(Stmt* s, EnvPtr env) {
    switch (s->kind) {
        case Stmt::VarDecl: {
            auto* v = static_cast<VarDeclStmt*>(s);
            for (auto& it : v->vars) {
                env->vars[it.name] = defaultOfType(it.type);
                env->moved.erase(it.name);
                if (it.type && (it.type->kind == Type::Named ||
                                it.type->kind == Type::OwnPtr)) {
                    env->owned.insert(it.name);
                }
            }
            return;
        }

        case Stmt::Assign: {
            auto* a = static_cast<AssignStmt*>(s);
            auto val = evalExpr(a->value.get(), env);
            assignTo(a->target.get(), val, env);
            return;
        }

        case Stmt::MovAssign: {
            auto* a = static_cast<MovAssignStmt*>(s);
            Value val;
            Value* srcSlot = nullptr;

            bool addr = (a->value->kind == Expr::Ident ||
                         a->value->kind == Expr::Member ||
                         a->value->kind == Expr::Index);
            if (addr) {
                srcSlot = lvalueOf(a->value.get(), env);
                val = *srcSlot;
            } else {
                val = evalExpr(a->value.get(), env);
            }

            assignTo(a->target.get(), val, env);
            if (srcSlot) *srcSlot = Value{};
            if (auto* srcId = dynamic_cast<IdentExpr*>(a->value.get()))
                env->markMoved(srcId->name);
            return;
        }

        case Stmt::SetAssign: {
            auto* a = static_cast<SetAssignStmt*>(s);
            auto val = evalExpr(a->value.get(), env);
            assignTo(a->target.get(), val, env);
            return;
        }

        case Stmt::IncDec: {
            auto* i = static_cast<IncDecStmt*>(s);
            auto* slot = lvalueOf(i->target.get(), env);
            if (isInt(*slot)) {
                *slot = Value(std::get<int64_t>(slot->v) + (i->isInc ? 1 : -1));
            } else if (isChr(*slot)) {
                uint8_t x = asChr(*slot);
                x = i->isInc ? static_cast<uint8_t>(x + 1)
                             : static_cast<uint8_t>(x - 1);
                *slot = Value(static_cast<char>(x));
            } else if (isFlt(*slot)) {
                *slot = Value(std::get<double>(slot->v) + (i->isInc ? 1.0 : -1.0));
            } else {
                throw ArcError::at(s->line, s->col, "inc/dec requires numeric");
            }
            return;
        }

        case Stmt::SwpStmt: {
            auto* sw = static_cast<SwpStmt*>(s);
            auto* a = lvalueOf(sw->lhs.get(), env);
            auto* b = lvalueOf(sw->rhs.get(), env);
            Value tmp = *a;
            *a = *b;
            *b = tmp;
            return;
        }

        case Stmt::ExprStmt:
            evalExpr(static_cast<ExprStmt*>(s)->expr.get(), env);
            return;

        case Stmt::IfStmt: {
            auto* i = static_cast<IfStmt*>(s);
            auto c = evalExpr(i->cond.get(), env);
            if (truthy(c)) execSuite(i->thenBody, env);
            else           execSuite(i->elseBody, env);
            return;
        }

        case Stmt::WhileStmt: {
            auto* w = static_cast<WhileStmt*>(s);
            while (truthy(evalExpr(w->cond.get(), env))) {
                try {
                    execSuite(w->body, env);
                } catch (BreakSignal& b) {
                    if (labelMatches(b.label, w->label)) break;
                    throw;
                } catch (ContinueSignal& c) {
                    if (labelMatches(c.label, w->label)) continue;
                    throw;
                }
            }
            return;
        }

        case Stmt::ForRange: {
            auto* f = static_cast<ForRangeStmt*>(s);

            if (f->isForEach) {
                auto iter = evalExpr(f->from.get(), env);
                if (!env->find(f->varName))
                    env->vars[f->varName] = Value{};

                if (isArr(iter)) {
                    auto arr = std::get<ArrayPtr>(iter.v);
                    auto snapshot = arr->elems;
                    for (auto& elem : snapshot) {
                        env->vars[f->varName] = elem;
                        try { execSuite(f->body, env); }
                        catch (BreakSignal& bs) {
                            if (labelMatches(bs.label, f->label)) break;
                            throw;
                        } catch (ContinueSignal& cs) {
                            if (labelMatches(cs.label, f->label)) continue;
                            throw;
                        }
                    }
                } else if (isSlice(iter)) {
                    auto sl = std::get<SlicePtr>(iter.v);
                    std::vector<Value> snapshot(
                        sl->source->elems.begin() + sl->offset,
                        sl->source->elems.begin() + sl->offset + sl->length);
                    for (auto& elem : snapshot) {
                        env->vars[f->varName] = elem;
                        try { execSuite(f->body, env); }
                        catch (BreakSignal& bs) {
                            if (labelMatches(bs.label, f->label)) break;
                            throw;
                        } catch (ContinueSignal& cs) {
                            if (labelMatches(cs.label, f->label)) continue;
                            throw;
                        }
                    }
                } else if (isMap(iter)) {
                    auto m = std::get<MapPtr>(iter.v);
                    std::vector<Value> keys;
                    for (auto& kv : m->data) {
                        std::visit([&](auto&& kk) {
                            using T = std::decay_t<decltype(kk)>;
                            if constexpr (std::is_same_v<T, std::string>)
                                keys.push_back(Value(kk));
                            else if constexpr (std::is_same_v<T, uint8_t>)
                                keys.push_back(Value(static_cast<char>(kk)));
                            else if constexpr (std::is_same_v<T, bool>)
                                keys.push_back(Value(kk));
                            else
                                keys.push_back(Value(static_cast<int64_t>(kk)));
                        }, kv.first.k);
                    }
                    for (auto& k : keys) {
                        env->vars[f->varName] = k;
                        try { execSuite(f->body, env); }
                        catch (BreakSignal& bs) {
                            if (labelMatches(bs.label, f->label)) break;
                            throw;
                        } catch (ContinueSignal& cs) {
                            if (labelMatches(cs.label, f->label)) continue;
                            throw;
                        }
                    }
                } else if (isRange(iter)) {
                    auto r = std::get<RangePtr>(iter.v);
                    auto fromVal = r->from;
                    auto toVal   = r->to;
                    bool excl    = r->exclusive;
                    int64_t step = 1;
                    if (r->hasStep) step = asInt(r->step);
                    if (step == 0)
                        throw ArcError::at(s->line, s->col, "for step cannot be 0");

                    bool useFloat = isFlt(fromVal) || isFlt(toVal);
                    if (useFloat) {
                        double a = asFlt(fromVal), b = asFlt(toVal);
                        auto go = [&](double x) {
                            if (excl) return step > 0 ? x < b : x > b;
                            return step > 0 ? x <= b : x >= b;
                        };
                        for (double x = a; go(x); x += step) {
                            env->vars[f->varName] = Value(x);
                            try { execSuite(f->body, env); }
                            catch (BreakSignal& bs) {
                                if (labelMatches(bs.label, f->label)) break;
                                throw;
                            } catch (ContinueSignal& cs) {
                                if (labelMatches(cs.label, f->label)) continue;
                                throw;
                            }
                        }
                    } else {
                        int64_t a = asInt(fromVal), b = asInt(toVal);
                        auto go = [&](int64_t x) {
                            if (excl) return step > 0 ? x < b : x > b;
                            return step > 0 ? x <= b : x >= b;
                        };
                        for (int64_t x = a; go(x); x += step) {
                            env->vars[f->varName] = Value(x);
                            try { execSuite(f->body, env); }
                            catch (BreakSignal& bs) {
                                if (labelMatches(bs.label, f->label)) break;
                                throw;
                            } catch (ContinueSignal& cs) {
                                if (labelMatches(cs.label, f->label)) continue;
                                throw;
                            }
                        }
                    }
                } else {
                    throw ArcError::at(s->line, s->col,
                        "for-each requires array, map, or range");
                }
                return;
            }

            auto fromV = evalExpr(f->from.get(), env);
            auto toV   = evalExpr(f->to.get(), env);
            int64_t step = 1;
            if (f->step) step = asInt(evalExpr(f->step.get(), env));
            if (step == 0)
                throw ArcError::at(s->line, s->col, "for step cannot be 0");

            bool useFloat = isFlt(fromV) || isFlt(toV);
            if (!env->find(f->varName))
                env->vars[f->varName] = Value(int64_t(0));

            if (useFloat) {
                double a = asFlt(fromV), b = asFlt(toV);
                auto go = [&](double x) {
                    if (f->exclusive) return step > 0 ? x < b : x > b;
                    return step > 0 ? x <= b : x >= b;
                };
                for (double x = a; go(x); x += step) {
                    env->vars[f->varName] = Value(x);
                    try {
                        execSuite(f->body, env);
                    } catch (BreakSignal& bs) {
                        if (labelMatches(bs.label, f->label)) break;
                        throw;
                    } catch (ContinueSignal& cs) {
                        if (labelMatches(cs.label, f->label)) continue;
                        throw;
                    }
                }
            } else {
                int64_t a = asInt(fromV), b = asInt(toV);
                auto go = [&](int64_t x) {
                    if (f->exclusive) return step > 0 ? x < b : x > b;
                    return step > 0 ? x <= b : x >= b;
                };
                for (int64_t x = a; go(x); x += step) {
                    env->vars[f->varName] = Value(x);
                    try {
                        execSuite(f->body, env);
                    } catch (BreakSignal& bs) {
                        if (labelMatches(bs.label, f->label)) break;
                        throw;
                    } catch (ContinueSignal& cs) {
                        if (labelMatches(cs.label, f->label)) continue;
                        throw;
                    }
                }
            }
            return;
        }

        case Stmt::ForC: {
            auto* f = static_cast<ForCStmt*>(s);
            auto loopEnv = std::make_shared<Env>(env);
            if (f->init) execStmt(f->init.get(), loopEnv);
            while (!f->cond || truthy(evalExpr(f->cond.get(), loopEnv))) {
                try {
                    execSuite(f->body, loopEnv);
                } catch (BreakSignal& bs) {
                    if (labelMatches(bs.label, f->label)) break;
                    throw;
                } catch (ContinueSignal& cs) {
                    if (!labelMatches(cs.label, f->label)) throw;
                }
                if (f->incr) execStmt(f->incr.get(), loopEnv);
            }
            return;
        }

        case Stmt::SwitchStmt: {
            auto* sw = static_cast<SwitchStmt*>(s);
            auto subj = evalExpr(sw->subject.get(), env);

            for (auto& c : sw->cases) {
                std::unordered_map<std::string, Value> bindings;
                if (!matchPattern(c.pattern, subj, bindings)) continue;

                auto caseEnv = std::make_shared<Env>(env);
                for (auto& kv : bindings) caseEnv->vars[kv.first] = kv.second;

                if (c.guard) {
                    auto g = evalExpr(c.guard.get(), caseEnv);
                    if (!truthy(g)) continue;
                }

                try {
                    execSuite(c.body, caseEnv);
                } catch (BreakSignal& bs) {
                    if (labelMatches(bs.label, sw->label)) return;
                    throw;
                } catch (ContinueSignal&) {
                    throw;
                }
                return;
            }

            if (sw->hasDefault) {
                try {
                    execSuite(sw->defBody, env);
                } catch (BreakSignal& bs) {
                    if (labelMatches(bs.label, sw->label)) return;
                    throw;
                }
            }
            return;
        }

        case Stmt::BreakStmt: {
            BreakSignal b;
            b.label = static_cast<BreakStmt*>(s)->label;
            throw b;
        }
        case Stmt::ContStmt: {
            ContinueSignal c;
            c.label = static_cast<ContStmt*>(s)->label;
            throw c;
        }

        case Stmt::JumpStmt:
            throw JumpSignal{static_cast<JumpStmt*>(s)->label};
        case Stmt::LabelStmt:
            return;

        case Stmt::RetStmt: {
            ReturnSignal r;
            auto* rs = static_cast<RetStmt*>(s);
            if (rs->value) r.value = evalExpr(rs->value.get(), env);
            throw r;
        }

        case Stmt::PrintStmt: {
            auto* p = static_cast<PrintStmt*>(s);
            std::ostream& os = (p->stream == PrintStmt::Stderr)
                               ? std::cerr : std::cout;
            for (auto& e : p->exprs) {
                os << valueToString(evalExpr(e.get(), env));
            }
            if (p->newline) os << "\n";
            return;
        }

        case Stmt::GetStmt: {
            auto* g = static_cast<GetStmt*>(s);
            for (auto& t : g->targets) {
                auto* slot = lvalueOf(t.get(), env);

                if (isInt(*slot)) {
                    int64_t x = 0;
                    if (!(std::cin >> x)) { x = 0; std::cin.clear(); }
                    *slot = Value(x);
                } else if (isFlt(*slot)) {
                    double x = 0;
                    if (!(std::cin >> x)) { x = 0; std::cin.clear(); }
                    *slot = Value(x);
                } else if (isStr(*slot)) {
                    std::string x;
                    std::cin >> x;
                    *slot = Value(x);
                } else if (isBol(*slot)) {
                    std::string tok;
                    std::cin >> tok;
                    if (tok == "1")      *slot = Value(true);
                    else if (tok == "0") *slot = Value(false);
                    else                 *slot = Value(false);
                } else if (isChr(*slot)) {
                    char c = 0;
                    std::cin >> std::ws;
                    std::cin.get(c);
                    if (!std::cin) { c = 0; std::cin.clear(); }
                    *slot = Value(c);
                }
            }
            return;
        }

        case Stmt::NewStmt:
            return;

        case Stmt::DelStmt: {
            auto* d = static_cast<DelStmt*>(s);
            auto* slot = env->find(d->varName);
            if (!slot) return;
            if (isObj(*slot)) {
                auto obj = std::get<ObjectPtr>(slot->v);
                auto fin = lookupMethod(obj->className, "fin");
                if (fin) {
                    std::vector<Value> args;
                    args.push_back(*slot);
                    callFunction(fin, std::move(args), env);
                }
            }
            *slot = Value{};
            return;
        }

        case Stmt::AtrStmt:
            throw ArcError::at(s->line, s->col, "atr outside coroutine");

        case Stmt::AttrStmt: {
            auto* a = static_cast<AttrStmt*>(s);
            if (a->attrName == "con") {
                env->markConst(a->varName);
            } else if (a->attrName == "ntr") {
                env->noRelease.insert(a->varName);
            }
            return;
        }

        case Stmt::AsmStmt: {
            auto* a = static_cast<AsmStmt*>(s);
            const std::string& txt = a->text;
            std::string expanded;
            std::vector<std::string> names;
            size_t i = 0;
            while (i < txt.size()) {
                if (txt[i] == '%' && i + 1 < txt.size() &&
                    (std::isalpha(static_cast<unsigned char>(txt[i + 1])) ||
                     txt[i + 1] == '_')) {
                    size_t j = i + 1;
                    while (j < txt.size() &&
                           (std::isalnum(static_cast<unsigned char>(txt[j])) ||
                            txt[j] == '_')) {
                        ++j;
                    }
                    std::string name = txt.substr(i + 1, j - i - 1);
                    names.push_back(name);
                    auto* slot = env->find(name);
                    if (slot) {
                        expanded += valueToString(*slot);
                    } else {
                        expanded += "<" + name + "?>";
                    }
                    i = j;
                } else {
                    expanded += txt[i++];
                }
            }
            std::cerr << "[asm line " << a->line << "] " << expanded << "\n";
            return;
        }

        case Stmt::UnwStmt: {
            auto* u = static_cast<UnwStmt*>(s);
            auto v = evalExpr(u->expr.get(), env);
            Value out;
            bool fromDefault = false;

            if (isNil(v)) fromDefault = true;
            else if (isOpt(v)) {
                auto o = std::get<OptPtr>(v.v);
                if (o && o->has) out = o->value;
                else fromDefault = true;
            } else if (isRlt(v)) {
                auto r = std::get<RltPtr>(v.v);
                if (r->isOk) out = r->value;
                else fromDefault = true;
            } else out = v;

            if (fromDefault)
                out = evalExpr(u->fallback.get(), env);
            env->vars[u->varName] = out;
            return;
        }

        case Stmt::BlockStmt: {
            auto* b = static_cast<BlockStmt*>(s);
            auto inner = std::make_shared<Env>(env);
            ScopeGuard _sg(this, inner);
            execSuite(b->body, inner);
            return;
        }
    }
    throw ArcError::at(s->line, s->col, "unknown statement kind");
}

inline Value* Interpreter::lvalueOf(Expr* target, EnvPtr env) {
    if (auto* id = dynamic_cast<IdentExpr*>(target)) {
        auto* slot = env->find(id->name);
        if (slot) return slot;
        env->vars[id->name] = Value{};
        return env->find(id->name);
    }
    if (auto* de = dynamic_cast<DerefExpr*>(target)) {
        auto pv = evalExpr(de->pointer.get(), env);
        if (!isPtr(pv))
            throw ArcError::at(target->line, target->col,
                "'*' / '^' requires a pointer operand");
        auto p = std::get<PtrPtr>(pv.v);
        if (!p || !p->target)
            throw ArcError::at(target->line, target->col,
                "null pointer dereference");
        return p->target;
    }
    if (auto* m = dynamic_cast<MemberExpr*>(target)) {
        auto recv = evalExpr(m->receiver.get(), env);
        if (isObj(recv)) {
            auto obj = std::get<ObjectPtr>(recv.v);
            return &obj->fields[m->name];
        }
        throw ArcError::at(target->line, target->col,
            "member assignment on non-object");
    }
    if (auto* ix = dynamic_cast<IndexExpr*>(target)) {
        auto cont = evalExpr(ix->container.get(), env);
        auto idx  = evalExpr(ix->index.get(), env);

        if (isArr(cont)) {
            auto arr = std::get<ArrayPtr>(cont.v);
            int64_t i = asInt(idx);
            if (i < 0 || i >= (int64_t)arr->elems.size())
                throw ArcError::at(target->line, target->col,
                    "array index out of range");
            return &arr->elems[static_cast<size_t>(i)];
        }
        if (isSlice(cont)) {
            throw ArcError::at(target->line, target->col,
                "slc<T> is a read-only view; cannot assign through it");
        }
        if (isMap(cont)) {
            auto m = std::get<MapPtr>(cont.v);
            auto k = valueToMapKeyAt(idx, target->line, target->col);
            return &m->data[k];
        }
        throw ArcError::at(target->line, target->col,
            "index assignment on non-array/map");
    }
    throw ArcError::at(target->line, target->col,
        "expression is not assignable");
}

inline void Interpreter::assignTo(Expr* target, const Value& val, EnvPtr env) {
    if (auto* id = dynamic_cast<IdentExpr*>(target)) {
        if (env->isConst(id->name))
            throw ArcError::at(target->line, target->col,
                "cannot assign to constant '" + id->name + "'");
    }
    auto* slot = lvalueOf(target, env);
    *slot = val;
    if (auto* id = dynamic_cast<IdentExpr*>(target))
        env->clearMoved(id->name);
}

inline bool Interpreter::matchPattern(
        const PatternPtr& p, const Value& v,
        std::unordered_map<std::string, Value>& b) {
    if (!p) return true;
    switch (p->kind) {
        case Pattern::Wild:
            return true;
        case Pattern::Bind:
            b[p->name] = v;
            return true;
        case Pattern::LitInt:
            return isInt(v) && std::get<int64_t>(v.v) == p->ival;
        case Pattern::LitFlt:
            return isFlt(v) && std::get<double>(v.v) == p->fval;
        case Pattern::LitChr:
            return isChr(v) && asChr(v) == static_cast<uint8_t>(p->cval);
        case Pattern::LitStr:
            return isStr(v) && std::get<std::string>(v.v) == p->sval;
        case Pattern::LitBol:
            return isBol(v) && std::get<bool>(v.v) == p->bval;
        case Pattern::LitNil:
            return isNil(v);
        case Pattern::Alias:
            b[p->name] = v;
            return matchPattern(p->aliasInner, v, b);
        case Pattern::EnmPattern: {
            if (!isObj(v)) return false;
            auto obj = std::get<ObjectPtr>(v.v);
            std::string want = p->typeName + "." + p->variant;
            if (obj->className != want) return false;
            auto it = obj->fields.find("payload");
            if (it == obj->fields.end()) return p->args.empty();
            if (!isArr(it->second)) return false;
            auto arr = std::get<ArrayPtr>(it->second.v);
            if (arr->elems.size() != p->args.size()) return false;
            for (size_t i = 0; i < p->args.size(); ++i)
                if (!matchPattern(p->args[i], arr->elems[i], b)) return false;
            return true;
        }
        case Pattern::RecPattern: {
            if (!isObj(v)) return false;
            auto obj = std::get<ObjectPtr>(v.v);
            if (obj->className != p->typeName) return false;
            for (auto& fp : p->fields) {
                auto it = obj->fields.find(fp.name);
                if (it == obj->fields.end()) return false;
                if (fp.pattern &&
                    !matchPattern(fp.pattern, it->second, b)) return false;
            }
            return true;
        }
        case Pattern::TupPattern: {
            if (!isArr(v)) return false;
            auto arr = std::get<ArrayPtr>(v.v);
            if (arr->elems.size() != p->args.size()) return false;
            for (size_t i = 0; i < p->args.size(); ++i)
                if (!matchPattern(p->args[i], arr->elems[i], b)) return false;
            return true;
        }
    }
    return false;
}

inline void Interpreter::destroyOwnedInScope(EnvPtr env) {
    if (!env) return;
    std::vector<std::string> names(env->owned.begin(), env->owned.end());
    env->owned.clear();
    for (auto& n : names) {
        if (env->noRelease.count(n)) continue;
        auto* slot = env->find(n);
        if (!slot) continue;
        if (!isObj(*slot)) continue;
        auto obj = std::get<ObjectPtr>(slot->v);
        if (!obj) continue;
        auto fin = lookupMethod(obj->className, "fin");
        if (fin) {
            std::vector<Value> args;
            args.push_back(*slot);
            try {
                callFunction(fin, std::move(args), env);
            } catch (...) {
            }
        }
        *slot = Value{};
    }
}

inline Value Interpreter::callFunction(std::shared_ptr<Function> fn,
                                       std::vector<Value> args,
                                       EnvPtr closureEnv) {
    if (!fn->hasBody)
        throw ArcError("function '" + fn->name +
                       "' is declared but not defined");

    size_t expected = fn->params.size();
    if (args.size() < expected) {
        for (size_t i = args.size(); i < expected; ++i) {
            auto& p = fn->params[i];
            if (!p.hasDefault)
                throw ArcError("function '" + fn->name +
                    "' expects " + std::to_string(expected) +
                    " arguments, got " + std::to_string(args.size()));
            args.push_back(evalExpr(p.defaultValue.get(), closureEnv));
        }
    } else if (args.size() > expected) {
        throw ArcError("function '" + fn->name +
            "' expects " + std::to_string(expected) +
            " arguments, got " + std::to_string(args.size()));
    }

    auto local = std::make_shared<Env>(closureEnv ? closureEnv : global_);
    local->globals = global_;

    ScopeGuard _sg(this, local);

    for (size_t i = 0; i < expected; ++i)
        local->vars[fn->params[i].name] = std::move(args[i]);

    try {
        execSuite(fn->body, local);
    } catch (ReturnSignal& r) {
        return r.value;
    } catch (TryReturnSignal& r) {
        if (fn->retType && fn->retType->kind == Type::Rlt)
            return Value(std::make_shared<RltValue>(false, r.error));
        throw ArcError("try propagates error but function '" + fn->name +
                       "' does not return rlt");
    } catch (JumpSignal& j) {
        throw ArcError("unknown label: " + j.label);
    }
    return Value{};
}

inline Value Interpreter::startCoroutine(std::shared_ptr<Function> fn,
                                         std::vector<Value> args,
                                         EnvPtr env) {
    auto co = std::make_shared<Coroutine>();
    co->fn = fn;

    size_t expected = fn->params.size();
    if (args.size() < expected) {
        for (size_t i = args.size(); i < expected; ++i) {
            auto& p = fn->params[i];
            if (!p.hasDefault)
                throw ArcError("coroutine '" + fn->name +
                    "' expects " + std::to_string(expected) +
                    " arguments, got " + std::to_string(args.size()));
            args.push_back(evalExpr(p.defaultValue.get(), env));
        }
    } else if (args.size() > expected) {
        throw ArcError("coroutine '" + fn->name +
            "' expects " + std::to_string(expected) +
            " arguments, got " + std::to_string(args.size()));
    }

    auto local = std::make_shared<Env>(env ? env : global_);
    local->globals = global_;
    for (size_t i = 0; i < expected; ++i)
        local->vars[fn->params[i].name] = std::move(args[i]);

    co->env = local;

    CoFrame root;
    root.kind = CoFrame::Block;
    root.body = &fn->body;
    root.pc   = 0;
    root.env  = local;
    co->stack.push_back(std::move(root));

    co->state = Coroutine::New;
    return Value(co);
}

namespace {
struct CoRunner {
    Interpreter* interp;
    bool execOne(CoroutinePtr co, Stmt* s, EnvPtr env);
};
}

inline bool CoRunner::execOne(CoroutinePtr co, Stmt* s, EnvPtr env) {
    switch (s->kind) {
        case Stmt::ExprStmt:
            interp->evalExpr(static_cast<ExprStmt*>(s)->expr.get(), env);
            return true;

        case Stmt::VarDecl: {
            auto* v = static_cast<VarDeclStmt*>(s);
            for (auto& it : v->vars)
                env->vars[it.name] = defaultOfType(it.type);
            return true;
        }

        case Stmt::Assign: {
            auto* a = static_cast<AssignStmt*>(s);
            auto val = interp->evalExpr(a->value.get(), env);
            interp->assignTo(a->target.get(), val, env);
            return true;
        }

        case Stmt::MovAssign: {
            auto* a = static_cast<MovAssignStmt*>(s);
            Value val;
            Value* srcSlot = nullptr;

            bool addr = (a->value->kind == Expr::Ident ||
                         a->value->kind == Expr::Member ||
                         a->value->kind == Expr::Index);
            if (addr) {
                srcSlot = interp->lvalueOf(a->value.get(), env);
                val = *srcSlot;
            } else {
                val = interp->evalExpr(a->value.get(), env);
            }

            interp->assignTo(a->target.get(), val, env);
            if (srcSlot) *srcSlot = Value{};
            if (auto* srcId = dynamic_cast<IdentExpr*>(a->value.get()))
                env->markMoved(srcId->name);
            return true;
        }

        case Stmt::SetAssign: {
            auto* a = static_cast<SetAssignStmt*>(s);
            auto val = interp->evalExpr(a->value.get(), env);
            interp->assignTo(a->target.get(), val, env);
            return true;
        }

        case Stmt::IncDec: {
            auto* i = static_cast<IncDecStmt*>(s);
            auto* slot = interp->lvalueOf(i->target.get(), env);
            if (isInt(*slot)) {
                *slot = Value(std::get<int64_t>(slot->v) + (i->isInc ? 1 : -1));
            } else if (isChr(*slot)) {
                uint8_t x = asChr(*slot);
                x = i->isInc ? static_cast<uint8_t>(x + 1)
                             : static_cast<uint8_t>(x - 1);
                *slot = Value(static_cast<char>(x));
            } else if (isFlt(*slot)) {
                *slot = Value(std::get<double>(slot->v) + (i->isInc ? 1.0 : -1.0));
            } else {
                throw ArcError::at(s->line, s->col, "inc/dec requires numeric");
            }
            return true;
        }

        case Stmt::SwpStmt: {
            auto* sw = static_cast<SwpStmt*>(s);
            auto* a = interp->lvalueOf(sw->lhs.get(), env);
            auto* b = interp->lvalueOf(sw->rhs.get(), env);
            Value tmp = *a;
            *a = *b;
            *b = tmp;
            return true;
        }

        case Stmt::IfStmt: {
            auto* i = static_cast<IfStmt*>(s);
            auto c = interp->evalExpr(i->cond.get(), env);
            const auto* body = truthy(c) ? &i->thenBody : &i->elseBody;
            if (!body->empty()) {
                CoFrame f;
                f.kind = CoFrame::Block;
                f.body = body;
                f.pc   = 0;
                f.env  = env;
                co->stack.push_back(std::move(f));
            }
            return true;
        }

        case Stmt::WhileStmt: {
            auto* w = static_cast<WhileStmt*>(s);
            CoFrame f;
            f.kind      = CoFrame::WhileHeader;
            f.cond      = w->cond.get();
            f.loopBody  = &w->body;
            f.env       = env;
            f.loopLabel = w->label;
            co->stack.push_back(std::move(f));
            return true;
        }

        case Stmt::ForRange: {
            auto* fr = static_cast<ForRangeStmt*>(s);
            if (fr->isForEach)
                throw ArcError::at(s->line, s->col,
                    "for-each not supported inside coroutine");
            auto fromV = interp->evalExpr(fr->from.get(), env);
            auto toV   = interp->evalExpr(fr->to.get(), env);

            CoFrame f;
            f.kind      = CoFrame::ForRangeHeader;
            f.varName   = fr->varName;
            f.exclusive = fr->exclusive;
            f.forBody   = &fr->body;
            f.env       = env;
            f.loopLabel = fr->label;

            bool useFloat = isFlt(fromV) || isFlt(toV);
            f.useFloat = useFloat;
            if (useFloat) {
                f.f64     = asFlt(fromV);
                f.f64End  = asFlt(toV);
                f.f64Step = fr->step ? asFlt(interp->evalExpr(fr->step.get(), env)) : 1.0;
                if (f.f64Step == 0)
                    throw ArcError::at(s->line, s->col, "for step cannot be 0");
            } else {
                f.i64     = asInt(fromV);
                f.i64End  = asInt(toV);
                f.i64Step = fr->step ? asInt(interp->evalExpr(fr->step.get(), env)) : 1;
                if (f.i64Step == 0)
                    throw ArcError::at(s->line, s->col, "for step cannot be 0");
            }
            if (!env->find(f.varName))
                env->vars[f.varName] = Value(int64_t(0));
            co->stack.push_back(std::move(f));
            return true;
        }

        case Stmt::ForC: {
            auto* fc = static_cast<ForCStmt*>(s);
            auto loopEnv = std::make_shared<Env>(env);
            if (fc->init) interp->execStmt(fc->init.get(), loopEnv);

            CoFrame f;
            f.kind      = CoFrame::ForCHeader;
            f.cCond     = fc->cond.get();
            f.cIncr     = fc->incr.get();
            f.cBody     = &fc->body;
            f.env       = loopEnv;
            f.loopLabel = fc->label;
            co->stack.push_back(std::move(f));
            return true;
        }

        case Stmt::SwitchStmt: {
            interp->execStmt(s, env);
            return true;
        }

        case Stmt::RetStmt: {
            auto* r = static_cast<RetStmt*>(s);
            co->returnValue = r->value
                ? interp->evalExpr(r->value.get(), env) : Value{};
            co->stack.clear();
            co->state = Coroutine::Done;
            return false;
        }

        case Stmt::BreakStmt: {
            std::string lbl = static_cast<BreakStmt*>(s)->label;
            for (size_t i = co->stack.size(); i-- > 0; ) {
                auto& f = co->stack[i];
                if (f.kind == CoFrame::WhileHeader ||
                    f.kind == CoFrame::ForRangeHeader ||
                    f.kind == CoFrame::ForCHeader) {
                    if (lbl.empty() || lbl == f.loopLabel) {
                        co->stack.resize(i);
                        return true;
                    }
                }
            }
            throw ArcError::at(s->line, s->col,
                "brk without matching loop in coroutine");
        }

        case Stmt::ContStmt: {
            std::string lbl = static_cast<ContStmt*>(s)->label;
            for (size_t i = co->stack.size(); i-- > 0; ) {
                auto& f = co->stack[i];
                if (f.kind == CoFrame::WhileHeader ||
                    f.kind == CoFrame::ForRangeHeader ||
                    f.kind == CoFrame::ForCHeader) {
                    if (lbl.empty() || lbl == f.loopLabel) {
                        co->stack.resize(i + 1);
                        return true;
                    }
                }
            }
            throw ArcError::at(s->line, s->col,
                "cnt without matching loop in coroutine");
        }

        case Stmt::JumpStmt:
        case Stmt::LabelStmt:
            throw ArcError::at(s->line, s->col,
                "jmp / label not supported inside coroutine");

        case Stmt::PrintStmt: {
            interp->execStmt(s, env);
            return true;
        }

        case Stmt::GetStmt:
            interp->execStmt(s, env);
            return true;

        case Stmt::AtrStmt: {
            auto* a = static_cast<AtrStmt*>(s);
            co->yieldValue = interp->evalExpr(a->value.get(), env);
            co->state = Coroutine::Suspended;
            throw SuspendSignal{};
        }

        case Stmt::NewStmt:
        case Stmt::DelStmt:
        case Stmt::AttrStmt:
            return true;

        case Stmt::AsmStmt:
            interp->execStmt(s, env);
            return true;

        case Stmt::UnwStmt:
            interp->execStmt(s, env);
            return true;

        case Stmt::BlockStmt: {
            auto* b = static_cast<BlockStmt*>(s);
            auto inner = std::make_shared<Env>(env);
            CoFrame f;
            f.kind = CoFrame::Block;
            f.body = &b->body;
            f.pc   = 0;
            f.env  = inner;
            co->stack.push_back(std::move(f));
            return true;
        }
    }
    throw ArcError::at(s->line, s->col,
        "unsupported statement in coroutine");
}

inline Value Interpreter::resumeCoroutine(CoroutinePtr co, const Value& /*in*/) {
    if (co->state == Coroutine::Done) return Value{};
    if (co->state == Coroutine::Failed) {
        if (co->err) std::rethrow_exception(co->err);
        return Value{};
    }

    co->state = Coroutine::Running;

    CoRunner runner{this};

    try {
        while (!co->stack.empty()) {
            auto& top = co->stack.back();

            if (top.kind == CoFrame::Block) {
                if (top.pc >= top.body->size()) {
                    co->stack.pop_back();
                    continue;
                }
                auto* s = (*top.body)[top.pc].get();
                top.pc++;
                bool cont = runner.execOne(co, s, top.env);
                if (!cont) break;
                continue;
            }

            if (top.kind == CoFrame::WhileHeader) {
                auto c = evalExpr(top.cond, top.env);
                if (truthy(c)) {
                    CoFrame f;
                    f.kind = CoFrame::Block;
                    f.body = top.loopBody;
                    f.pc   = 0;
                    f.env  = top.env;
                    co->stack.push_back(std::move(f));
                } else {
                    co->stack.pop_back();
                }
                continue;
            }

            if (top.kind == CoFrame::ForRangeHeader) {
                bool go = false;
                if (top.useFloat) {
                    if (top.exclusive)
                        go = top.f64Step > 0 ? top.f64 < top.f64End
                                             : top.f64 > top.f64End;
                    else
                        go = top.f64Step > 0 ? top.f64 <= top.f64End
                                             : top.f64 >= top.f64End;
                    if (go) {
                        top.env->vars[top.varName] = Value(top.f64);
                        top.f64 += top.f64Step;
                    }
                } else {
                    if (top.exclusive)
                        go = top.i64Step > 0 ? top.i64 < top.i64End
                                             : top.i64 > top.i64End;
                    else
                        go = top.i64Step > 0 ? top.i64 <= top.i64End
                                             : top.i64 >= top.i64End;
                    if (go) {
                        top.env->vars[top.varName] = Value(top.i64);
                        top.i64 += top.i64Step;
                    }
                }
                if (go) {
                    CoFrame f;
                    f.kind = CoFrame::Block;
                    f.body = top.forBody;
                    f.pc   = 0;
                    f.env  = top.env;
                    co->stack.push_back(std::move(f));
                } else {
                    co->stack.pop_back();
                }
                continue;
            }

            if (top.kind == CoFrame::ForCHeader) {
                if (top.cIncrPending) {
                    if (top.cIncr) execStmt(top.cIncr, top.env);
                } else {
                    top.cIncrPending = true;
                }

                bool go = true;
                if (top.cCond) {
                    auto c = evalExpr(top.cCond, top.env);
                    go = truthy(c);
                }
                if (go) {
                    CoFrame f;
                    f.kind = CoFrame::Block;
                    f.body = top.cBody;
                    f.pc   = 0;
                    f.env  = top.env;
                    co->stack.push_back(std::move(f));
                } else {
                    co->stack.pop_back();
                }
                continue;
            }
        }
    } catch (SuspendSignal&) {
        return co->yieldValue;
    } catch (...) {
        co->state = Coroutine::Failed;
        co->err = std::current_exception();
        throw;
    }

    if (co->state == Coroutine::Running)
        co->state = Coroutine::Done;
    return co->returnValue;
}

// ============================================================================
// P11  Interpreter::run()
// ============================================================================

inline int Interpreter::run(int argc, char** argv) {
    if (!prog_) throw ArcError("no program");
    if (!prog_->entry)
        throw ArcError("no entry point ('ent main')");

    global_ = std::make_shared<Env>();
    global_->globals = global_;

    moduleEnvs_.clear();
    auto& moduleEnvs = moduleEnvs_;
    for (auto& kv : prog_->modules) {
        auto env = std::make_shared<Env>(global_);
        env->globals = global_;
        moduleEnvs[kv.first] = env;
    }

    std::unordered_set<std::string> registered;
    for (auto& tl : prog_->topLevel) {
        if (tl.kind == TopLevel::Fun && tl.fun) {
            if (!tl.fun->ownerModule.empty()) continue;
            std::string regName = tl.fun->name;
            if (registered.count(regName)) continue;
            auto cl = std::make_shared<Closure>(tl.fun, global_);
            global_->vars[regName] = Value(cl);
            registered.insert(regName);
        }
    }
    for (auto& kv : prog_->functions) {
        if (registered.count(kv.first)) continue;
        EnvPtr closureEnv = global_;
        if (!kv.second->ownerModule.empty()) {
            auto mit = moduleEnvs.find(kv.second->ownerModule);
            if (mit != moduleEnvs.end()) closureEnv = mit->second;
        }
        auto cl = std::make_shared<Closure>(kv.second, closureEnv);
        global_->vars[kv.first] = Value(cl);
        registered.insert(kv.first);
    }

    for (auto& kv : prog_->modules) {
        auto mit = moduleEnvs.find(kv.first);
        if (mit == moduleEnvs.end()) continue;
        EnvPtr modEnv = mit->second;
        for (auto& full : kv.second->members) {
            auto pos = full.find('$');
            if (pos == std::string::npos) continue;
            std::string shortName = full.substr(pos + 1);
            auto fit = prog_->functions.find(full);
            if (fit != prog_->functions.end()) {
                auto cl = std::make_shared<Closure>(fit->second, modEnv);
                modEnv->vars[shortName] = Value(cl);
            }
        }
    }

    for (auto& kv : prog_->useAliases) {
        auto it = prog_->functions.find(kv.second);
        if (it != prog_->functions.end()) {
            EnvPtr closureEnv = global_;
            if (!it->second->ownerModule.empty()) {
                auto mit = moduleEnvs.find(it->second->ownerModule);
                if (mit != moduleEnvs.end()) closureEnv = mit->second;
            }
            auto cl = std::make_shared<Closure>(it->second, closureEnv);
            global_->vars[kv.first] = Value(cl);
        }
    }

    for (auto& tl : prog_->topLevel) {
        if (tl.kind != TopLevel::GlobalVar || !tl.stmt) continue;
        auto* vd = dynamic_cast<VarDeclStmt*>(tl.stmt.get());
        if (!vd) continue;
        for (auto& it : vd->vars) {
            if (!it.ownerModule.empty()) {
                auto mit = moduleEnvs.find(it.ownerModule);
                if (mit != moduleEnvs.end()) {
                    mit->second->vars[it.name] = defaultOfType(it.type);
                    continue;
                }
            }
            global_->vars[it.name] = defaultOfType(it.type);
        }
    }

    for (auto& tl : prog_->topLevel) {
        if (tl.kind != TopLevel::Equ || !tl.stmt) continue;
        auto* es = dynamic_cast<ExprStmt*>(tl.stmt.get());
        if (!es) continue;
        Value v = evalExpr(es->expr.get(), global_);
        global_->vars[tl.s1] = v;
    }

    for (auto& kv : prog_->consts) {
        if (!global_->vars.count(kv.first))
            global_->vars[kv.first] = Value(kv.second);
    }

    try {
        std::vector<Value> entryArgs;
        if (!prog_->entry->params.empty()) {
            std::vector<Value> strs;
            for (int i = 0; i < argc; ++i)
                strs.push_back(Value(std::string(argv ? argv[i] : "")));
            entryArgs.push_back(Value(std::make_shared<Array>(std::move(strs))));
        }
        Value result = callFunction(prog_->entry, std::move(entryArgs), global_);
        if (isInt(result))
            return static_cast<int>(std::get<int64_t>(result.v));
        if (isBol(result))
            return std::get<bool>(result.v) ? 1 : 0;
        if (isChr(result))
            return static_cast<int>(asChr(result));
        return 0;
    } catch (TryReturnSignal& r) {
        std::cerr << "arc: uncaught error: " << valueToString(r.error) << "\n";
        return 1;
    }
}

// ============================================================================
// P12  后端抽象骨架 + NASM PoC
// ============================================================================
enum class RetKind { Reg, RegPair, HiddenPtr };

class Target {
public:
    virtual ~Target() = default;
    virtual std::string name()      const = 0;
    virtual std::string asmFormat() const = 0;
    virtual std::string objExt()    const = 0;
    virtual std::string outExt()    const = 0;

    virtual std::string intArgReg(int i)   const = 0;
    virtual std::string floatArgReg(int i) const = 0;

    virtual RetKind retKind(int64_t sz)    const = 0;
    virtual std::string hiddenRetPtrReg()  const = 0;

    virtual std::vector<std::string> calleeSaved() const = 0;
    virtual int stackAlign()  const = 0;
    virtual int shadowSpace() const = 0;

    virtual int stackIntArgOffset(int i) const = 0;
    virtual int stackFltArgOffset(int i) const = 0;

    virtual std::vector<std::string> assembleArgs(
        const std::string& a, const std::string& o) const = 0;
    virtual std::vector<std::string> linkArgs(
        const std::string& o, const std::string& e) const = 0;

    virtual std::string runtimeLabel(const std::string& fn) const {
        return "__arc_" + fn;
    }
    virtual std::string entryLabel() const = 0;
    virtual bool needsUnwindInfo() const { return false; }

    virtual int intArgRegs() const {
        int n = 0;
        while (!intArgReg(n).empty()) ++n;
        return n;
    }
};

class LinuxX64Target : public Target {
public:
    std::string name()      const override { return "linux-x86_64"; }
    std::string asmFormat() const override { return "elf64"; }
    std::string objExt()    const override { return ".o"; }
    std::string outExt()    const override { return ""; }

    std::string intArgReg(int i) const override {
        static const char* r[] = {"rdi","rsi","rdx","rcx","r8","r9"};
        return (i >= 0 && i < 6) ? r[i] : "";
    }
    std::string floatArgReg(int i) const override {
        return (i >= 0 && i < 8) ? ("xmm" + std::to_string(i)) : "";
    }
    RetKind retKind(int64_t sz) const override {
        if (sz <= 8)  return RetKind::Reg;
        if (sz <= 16) return RetKind::RegPair;
        return RetKind::HiddenPtr;
    }
    std::string hiddenRetPtrReg() const override { return "rdi"; }
    std::vector<std::string> calleeSaved() const override {
        return {"rbx","rbp","r12","r13","r14","r15"};
    }
    int stackAlign()  const override { return 16; }
    int shadowSpace() const override { return 0; }

    int stackIntArgOffset(int i) const override { return 16 + (i - 6) * 8; }
    int stackFltArgOffset(int i) const override { return 16 + (i - 8) * 8; }

    std::vector<std::string> assembleArgs(
        const std::string& a, const std::string& o) const override {
        return {"nasm", "-f", "elf64", a, "-o", o};
    }
    std::vector<std::string> linkArgs(
        const std::string& o, const std::string& e) const override {
        return {"ld", o, "-o", e};
    }
    std::string entryLabel() const override { return "_start"; }
};

class WinX64Target : public Target {
public:
    std::string name()      const override { return "win-x86_64"; }
    std::string asmFormat() const override { return "win64"; }
    std::string objExt()    const override { return ".obj"; }
    std::string outExt()    const override { return ".exe"; }

    std::string intArgReg(int i) const override {
        static const char* r[] = {"rcx","rdx","r8","r9"};
        return (i >= 0 && i < 4) ? r[i] : "";
    }
    std::string floatArgReg(int i) const override {
        return (i >= 0 && i < 4) ? ("xmm" + std::to_string(i)) : "";
    }
    RetKind retKind(int64_t sz) const override {
        if (sz <= 8)  return RetKind::Reg;
        return RetKind::HiddenPtr;
    }
    std::string hiddenRetPtrReg() const override { return "rcx"; }
    std::vector<std::string> calleeSaved() const override {
        return {"rbx","rbp","rdi","rsi","r12","r13","r14","r15"};
    }
    int stackAlign()  const override { return 16; }
    int shadowSpace() const override { return 32; }

    int stackIntArgOffset(int i) const override { return 48 + (i - 4) * 8; }
    int stackFltArgOffset(int i) const override { return 48 + (i - 4) * 8; }

    std::vector<std::string> assembleArgs(
        const std::string& a, const std::string& o) const override {
        return {"nasm", "-f", "win64", a, "-o", o};
    }
    std::vector<std::string> linkArgs(
        const std::string& o, const std::string& e) const override {
        return {"lld-link", "/subsystem:console",
                "/entry:mainCRTStartup",
                "/out:" + e, o,
                "msvcrt.lib", "kernel32.lib"};
    }
    std::string entryLabel() const override { return "mainCRTStartup"; }
    bool needsUnwindInfo() const override { return true; }
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual int compile(Program& prog,
                        const std::vector<std::string>& args) = 0;
};

class InterpreterBackend : public Backend {
public:
    int compile(Program& prog,
                const std::vector<std::string>& args) override {
        auto sp = std::shared_ptr<Program>(&prog, [](Program*){});
        Interpreter interp(sp);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        return interp.run(static_cast<int>(args.size()), argv.data());
    }
};

class NasmBackend : public Backend {
public:
    NasmBackend(Target* t, const std::string& outPath)
        : target_(t), outPath_(outPath) {}

    int compile(Program& prog, const std::vector<std::string>&) override {
        std::ostringstream out;
        emitProgram(prog, out);
        if (outPath_.empty()) {
            std::cout << out.str();
        } else {
            std::ofstream f(outPath_);
            if (!f) throw ArcError("cannot write " + outPath_);
            f << out.str();
        }
        return 0;
    }

private:
    Target* target_;
    std::string outPath_;
    int rodataCounter_ = 0;
    int labelCounter_ = 0;
    std::map<std::string, int> locals_;   // 变量 → rbp 偏移（负）
    std::set<std::string> globalNames_;   // 全局变量名
    struct LoopEntry { std::string startLbl; std::string endLbl; std::string label; std::string contLbl; };
    std::vector<LoopEntry> loopStack_;
    int nextLocalOffset_ = 0;
    int frameSize_ = 0;
    std::map<std::string, int64_t> arrayN_;
    std::set<std::string> arrParamNames_;
    std::set<std::string> fltVars_;
    std::set<std::string> strVars_;
    std::ostringstream* curRodata_ = nullptr;

    int countLocalBytes(const std::vector<StmtPtr>& stmts) {
        int n = 0;
        for (auto& s : stmts) {
            if (auto* vd = dynamic_cast<VarDeclStmt*>(s.get())) {
                for (auto& it : vd->vars) {
                    if (it.type && it.type->kind == Type::Arr)
                        n += static_cast<int>(it.type->arraySize) * 8;
                    else if (it.type && it.type->kind == Type::Str)
                        n += 16;
                    else
                        n += 8;
                }
            }
            else if (auto* i = dynamic_cast<IfStmt*>(s.get())) {
                n += countLocalBytes(i->thenBody);
                n += countLocalBytes(i->elseBody);
            }
            else if (auto* w = dynamic_cast<WhileStmt*>(s.get()))
                n += countLocalBytes(w->body);
            else if (auto* fr = dynamic_cast<ForRangeStmt*>(s.get())) {
                if (!fr->isForEach) n += 8;
                n += countLocalBytes(fr->body);
            }
            else if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
                for (auto& cc : sw->cases) n += countLocalBytes(cc.body);
                n += countLocalBytes(sw->defBody);
            }
        }
        return n;
    }

    void emitFunction(Function& fn, std::ostringstream& out,
                      std::ostringstream& rodata) {
        curRodata_ = &rodata;
        std::string label = "arc_fn_" + fn.name;
        out << label << ":\n";
        out << "    push rbp\n";
        out << "    mov rbp, rsp\n";

        locals_.clear();
        arrayN_.clear();
        strVars_.clear();
        arrParamNames_.clear();
        fltVars_.clear();
        nextLocalOffset_ = 0;
        int paramCount = static_cast<int>(fn.params.size());
        for (int i = 0; i < paramCount; ++i) {
            nextLocalOffset_ -= 8;
            locals_[fn.params[i].name] = nextLocalOffset_;
        }

        int extraBytes = countLocalBytes(fn.body);
        int totalBytes = paramCount * 8 + extraBytes;
        int frameSize = alignUp16(totalBytes + 8);
        if (frameSize < 16) frameSize = 16;
        frameSize_ = frameSize;
        out << "    sub rsp, " << frameSize << "\n";
        out << "    call __arc_strbuf_save\n";
        out << "    mov [rbp - " << frameSize << "], rax\n";

        int intRegCount = target_->intArgRegs();
        int intIdx = 0, fltIdx = 0;
        for (int i = 0; i < paramCount; ++i) {
            int off = locals_[fn.params[i].name];
            bool isFltP = fn.params[i].type && fn.params[i].type->kind == Type::Flt;
            if (isFltP) {
                fltVars_.insert(fn.params[i].name);
                std::string xmm = target_->floatArgReg(fltIdx++);
                if (xmm.empty())
                    throw ArcError("NASM backend: too many flt params in " + fn.name);
                out << "    movsd [rbp " << off << "], " << xmm << "\n";
            } else {
                if (intIdx < intRegCount) {
                    std::string reg = target_->intArgReg(intIdx);
                    out << "    mov [rbp " << off << "], " << reg << "\n";
                } else {
                    int stackOff = 16 + target_->shadowSpace() + (intIdx - intRegCount) * 8;
                    out << "    mov rax, [rbp + " << stackOff << "]\n";
                    out << "    mov [rbp " << off << "], rax\n";
                }
                if (fn.params[i].type && fn.params[i].type->kind == Type::Arr)
                    arrParamNames_.insert(fn.params[i].name);
                ++intIdx;
            }
        }

        emitBody(fn.body, out, rodata);

        out << "    xor eax, eax\n";
        out << "    mov r11, [rbp - " << frameSize << "]\n";
        out << "    mov [rel __arc_strbuf_used], r11\n";
        out << "    leave\n";
        out << "    ret\n\n";
    }

    void emitProgram(Program& prog, std::ostringstream& out) {
        if (!prog.entry)
            throw ArcError("NASM backend: no entry point");
        std::ostringstream funcs;
        std::ostringstream rodata;
        std::ostringstream dataSec;
        rodataCounter_ = 0;
        labelCounter_ = 0;
        globalNames_.clear();
        for (auto& tl : prog.topLevel) {
            if (tl.kind != TopLevel::GlobalVar || !tl.stmt) continue;
            auto* vd = dynamic_cast<VarDeclStmt*>(tl.stmt.get());
            if (!vd) continue;
            for (auto& it : vd->vars) {
                if (!it.ownerModule.empty()) continue;
                globalNames_.insert(it.name);
                dataSec << "g_" << it.name << ": dq 0\n";
            }
        }

        {
            std::string saved = prog.entry->name;
            prog.entry->name = "main";
            emitFunction(*prog.entry, funcs, rodata);
            prog.entry->name = saved;
        }

        for (auto& kv : prog.functions) {
            if (!kv.second) continue;
            if (kv.second.get() == prog.entry.get()) continue;
            if (!kv.second->hasBody) continue;
            emitFunction(*kv.second, funcs, rodata);
        }

        if (target_->name() == "linux-x86_64") {
            out << "; Linux x86_64 -- NASM\n";
            out << "section .text\n";
            out << "global _start\n";
            out << "_start:\n";
            out << "    call arc_fn_main\n";
            out << "    mov rdi, rax\n";
            out << "    mov rax, 60\n";
            out << "    syscall\n\n";
            out << funcs.str();
            emitLinuxRuntime(out);
            if (!dataSec.str().empty()) {
                out << "section .data\n";
                out << dataSec.str();
            }
            emitStrRuntime(out);
            out << "section .bss\n";
            out << "__arc_strbuf: resb 1048576\n";
            out << "__arc_strbuf_used: resq 1\n";
            out << "__arc_its_buf: resb 32\n";
            out << "section .rodata\n";
            out << rodata.str();
            out << "__arc_str_tru: db \"tru\", 0\n";
            out << "__arc_str_fal: db \"fal\", 0\n";
            out << "__arc_nl_char: db 10\n";
            out << "__arc_empty_str: db 0\n";
            out << "__arc_dot: db \".\"\n";
            out << "__arc_minus: db \"-\"\n";
            out << "__arc_errbuf_over: db \"arc: string buffer overflow\", 10\n";
            out << "__arc_errbuf_over_len equ $ - __arc_errbuf_over\n";
        } else if (target_->name() == "win-x86_64") {
            out << "; Windows x86_64 -- NASM\n";
            out << "section .text\n";
            out << "global mainCRTStartup\n";
            out << "extern printf\n";
            out << "extern scanf\n";
            out << "extern exit\n";
            out << "mainCRTStartup:\n";
            out << "    sub rsp, 40\n";
            out << "    call arc_fn_main\n";
            out << "    mov ecx, eax\n";
            out << "    call exit\n\n";
            out << funcs.str();
            if (!dataSec.str().empty()) {
                out << "section .data\n";
                out << dataSec.str();
            }
            emitStrRuntime(out);
            out << "section .bss\n";
            out << "__arc_strbuf: resb 1048576\n";
            out << "__arc_strbuf_used: resq 1\n";
            out << "__arc_its_buf: resb 32\n";
            out << "section .rdata\n";
            out << rodata.str();
            out << "__arc_fmt_str: db \"%s\", 0\n";
            out << "__arc_fmt_int: db \"%d\", 0\n";
            out << "__arc_fmt_scanint: db \"%d\", 0\n";
            out << "__arc_fmt_strn: db \"%.*s\", 0\n";
            out << "__arc_nl: db 10, 0\n";
            out << "__arc_brk_open: db \"[\", 0\n";
            out << "__arc_brk_comma: db \", \", 0\n";
            out << "__arc_brk_close: db \"]\", 0\n";
            out << "__arc_brk_close_nl: db \"]\", 10, 0\n";
            out << "__arc_empty_str: db 0\n";
            out << "__arc_str_tru: db \"tru\", 0\n";
            out << "__arc_str_fal: db \"fal\", 0\n";
            out << "__arc_fmt_flt: db \"%f\", 0\n";
            out << "__arc_errbuf_over: db \"arc: string buffer overflow\", 0\n";
            out << "__arc_errbuf_over_len equ $ - __arc_errbuf_over - 1\n";
        } else {
            throw ArcError("unknown target: " + target_->name());
        }
    }

    static int alignUp16(int n) {
        return (n + 15) & ~15;
    }

    bool isFloatExpr(Expr* e) {
        if (!e) return false;
        if (dynamic_cast<FltExpr*>(e)) return true;
        if (auto* id = dynamic_cast<IdentExpr*>(e))
            return fltVars_.count(id->name) > 0;
        if (auto* u = dynamic_cast<UnaryExpr*>(e)) {
            if (u->op == "-") return isFloatExpr(u->operand.get());
            return false;
        }
        if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
            const std::string& o = b->op;
            if (o == "+" || o == "-" || o == "*" || o == "/")
                return isFloatExpr(b->lhs.get()) || isFloatExpr(b->rhs.get());
        }
        return false;
    }

    void emitFloatToXmm0(Expr* e, std::ostringstream& out) {
        if (auto* fe = dynamic_cast<FltExpr*>(e)) {
            uint64_t bits;
            double v = fe->v;
            std::memcpy(&bits, &v, 8);
            out << "    mov rax, 0x" << std::hex << bits << std::dec << "\n";
            out << "    movq xmm0, rax\n";
            return;
        }
        if (auto* id = dynamic_cast<IdentExpr*>(e)) {
            auto it = locals_.find(id->name);
            if (it == locals_.end())
                throw ArcError("NASM backend: float var not found: " + id->name);
            out << "    movsd xmm0, [rbp " << it->second << "]\n";
            return;
        }
        if (auto* u = dynamic_cast<UnaryExpr*>(e)) {
            if (u->op == "-") {
                emitFloatToXmm0(u->operand.get(), out);
                out << "    mov rax, 0x8000000000000000\n";
                out << "    movq xmm1, rax\n";
                out << "    xorpd xmm0, xmm1\n";
                return;
            }
        }
        if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
            const std::string& o = b->op;
            if (o == "+" || o == "-" || o == "*" || o == "/") {
                emitFloatToXmm0(b->lhs.get(), out);
                out << "    sub rsp, 8\n";
                out << "    movsd [rsp], xmm0\n";
                emitFloatToXmm0(b->rhs.get(), out);
                out << "    movsd xmm1, xmm0\n";
                out << "    movsd xmm0, [rsp]\n";
                out << "    add rsp, 8\n";
                if (o == "+")      out << "    addsd xmm0, xmm1\n";
                else if (o == "-") out << "    subsd xmm0, xmm1\n";
                else if (o == "*") out << "    mulsd xmm0, xmm1\n";
                else if (o == "/") out << "    divsd xmm0, xmm1\n";
                return;
            }
        }
        if (dynamic_cast<IntExpr*>(e)) {
            emitExprTo(e, out, "rax");
            out << "    cvtsi2sd xmm0, rax\n";
            return;
        }
        throw ArcError("NASM backend: unsupported float expr");
    }

    void emitStrPartToRegs(Expr* e, std::ostringstream& out,
                           std::ostringstream& rodata,
                           const std::string& ptrReg,
                           const std::string& lenReg) {
        if (auto* se = dynamic_cast<StrExpr*>(e)) {
            int rid = rodataCounter_++;
            std::string lbl = "str" + std::to_string(rid);
            rodata << lbl << ": db ";
            for (unsigned char c : se->v) {
                if (c == '"') rodata << "34, ";
                else if (c == '\\') rodata << "92, ";
                else if (c >= 32 && c < 127) rodata << "'" << c << "', ";
                else rodata << static_cast<int>(c) << ", ";
            }
            rodata << "0\n";
            out << "    lea " << ptrReg << ", [rel " << lbl << "]\n";
            out << "    mov " << lenReg << ", " << se->v.size() << "\n";
            return;
        }
        if (auto* id = dynamic_cast<IdentExpr*>(e)) {
            auto sit = locals_.find(id->name);
            if (sit != locals_.end() && strVars_.count(id->name)) {
                out << "    mov " << ptrReg << ", [rbp " << sit->second << "]\n";
                out << "    mov " << lenReg << ", [rbp " << (sit->second + 8) << "]\n";
                return;
            }
        }
        emitExprTo(e, out, "rdi");
        out << "    call __arc_int_to_str\n";
        out << "    mov r11, rdx\n";
        out << "    mov " << ptrReg << ", rax\n";
        out << "    mov " << lenReg << ", r11\n";
    }

    void emitExprTo(Expr* e, std::ostringstream& out, const std::string& dst) {
        if (isFloatExpr(e)) {
            emitFloatToXmm0(e, out);
            out << "    movq rax, xmm0\n";
            if (dst != "rax") out << "    mov " << dst << ", rax\n";
            return;
        }
        if (auto* ie = dynamic_cast<IntExpr*>(e)) {
            out << "    mov " << dst << ", " << ie->v << "\n";
            return;
        }
        if (auto* id = dynamic_cast<IdentExpr*>(e)) {
            auto it = locals_.find(id->name);
            if (it != locals_.end()) {
                out << "    mov " << dst << ", [rbp " << it->second << "]\n";
                return;
            }
            if (globalNames_.count(id->name)) {
                out << "    mov " << dst << ", [rel g_" << id->name << "]\n";
                return;
            }
            throw ArcError("NASM backend: undefined variable: " + id->name);
        }
        if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
            if (b->op == "==" || b->op == "!=") {
                auto isStrE = [&](Expr* x) -> bool {
                    if (dynamic_cast<StrExpr*>(x)) return true;
                    if (auto* id2 = dynamic_cast<IdentExpr*>(x))
                        return strVars_.count(id2->name) > 0;
                    return false;
                };
                if (isStrE(b->lhs.get()) || isStrE(b->rhs.get())) {
                    if (!curRodata_)
                        throw ArcError("NASM backend: str cmp requires rodata context");
                    auto ldS = [&](Expr* x, const std::string& pR,
                                   const std::string& lR) {
                        if (auto* se2 = dynamic_cast<StrExpr*>(x)) {
                            int rid = rodataCounter_++;
                            std::string lbl = "str" + std::to_string(rid);
                            (*curRodata_) << lbl << ": db ";
                            for (unsigned char c : se2->v) {
                                if (c == '"') (*curRodata_) << "34, ";
                                else if (c == '\\') (*curRodata_) << "92, ";
                                else if (c >= 32 && c < 127) (*curRodata_) << "'" << c << "', ";
                                else (*curRodata_) << static_cast<int>(c) << ", ";
                            }
                            (*curRodata_) << "0\n";
                            out << "    lea " << pR << ", [rel " << lbl << "]\n";
                            out << "    mov " << lR << ", " << se2->v.size() << "\n";
                            return;
                        }
                        if (auto* id2 = dynamic_cast<IdentExpr*>(x)) {
                            auto sit2 = locals_.find(id2->name);
                            if (sit2 == locals_.end() || !strVars_.count(id2->name))
                                throw ArcError("NASM backend: str cmp needs str operands");
                            out << "    mov " << pR << ", [rbp " << sit2->second << "]\n";
                            out << "    mov " << lR << ", [rbp " << (sit2->second + 8) << "]\n";
                            return;
                        }
                        throw ArcError("NASM backend: str cmp needs str operands");
                    };
                    ldS(b->lhs.get(), "rdi", "rsi");
                    ldS(b->rhs.get(), "rdx", "rcx");
                    out << "    call __arc_str_eq\n";
                    if (b->op == "!=") out << "    xor rax, 1\n";
                    if (dst != "rax") out << "    mov " << dst << ", rax\n";
                    return;
                }
            }
            emitExprTo(b->lhs.get(), out, "rax");
            out << "    push rax\n";
            emitExprTo(b->rhs.get(), out, "rax");
            out << "    mov rcx, rax\n";
            out << "    pop rax\n";
            std::string op = b->op;
            if (op == "+") out << "    add rax, rcx\n";
            else if (op == "-") out << "    sub rax, rcx\n";
            else if (op == "*") out << "    imul rax, rcx\n";
            else if (op == "/") {
                out << "    cqo\n";
                out << "    idiv rcx\n";
            }
            else if (op == "%") {
                out << "    cqo\n";
                out << "    idiv rcx\n";
                out << "    mov rax, rdx\n";
            }
            else if (op == "==" || op == "!=" || op == "<" ||
                     op == ">"  || op == "<=" || op == ">=") {
                out << "    cmp rax, rcx\n";
                if      (op == "==") out << "    sete al\n";
                else if (op == "!=") out << "    setne al\n";
                else if (op == "<")  out << "    setl al\n";
                else if (op == ">")  out << "    setg al\n";
                else if (op == "<=") out << "    setle al\n";
                else                 out << "    setge al\n";
                out << "    movzx rax, al\n";
            }
            else throw ArcError("NASM backend: unsupported binary op: " + op);
            if (dst != "rax") out << "    mov " << dst << ", rax\n";
            return;
        }
        if (auto* ix = dynamic_cast<IndexExpr*>(e)) {
            auto* id = dynamic_cast<IdentExpr*>(ix->container.get());
            if (!id)
                throw ArcError("NASM backend: index only on array variable");
            auto it = locals_.find(id->name);
            if (it == locals_.end() ||
                (!arrayN_.count(id->name) && !arrParamNames_.count(id->name)))
                throw ArcError("NASM backend: not an array: " + id->name);
            emitExprTo(ix->index.get(), out, "rax");
            if (arrParamNames_.count(id->name))
                out << "    mov rcx, [rbp " << it->second << "]\n";
            else
                out << "    lea rcx, [rbp " << it->second << "]\n";
            out << "    mov " << dst << ", [rcx + rax * 8]\n";
            return;
        }
        if (auto* u = dynamic_cast<UnaryExpr*>(e)) {
            if (u->op == "#") {
                if (auto* id = dynamic_cast<IdentExpr*>(u->operand.get())) {
                    auto it = arrayN_.find(id->name);
                    if (it != arrayN_.end()) {
                        out << "    mov " << dst << ", " << it->second << "\n";
                        return;
                    }
                    if (strVars_.count(id->name)) {
                        auto sit = locals_.find(id->name);
                        if (sit == locals_.end())
                            throw ArcError("NASM backend: str var not found");
                        out << "    mov " << dst << ", [rbp " << (sit->second + 8) << "]\n";
                        return;
                    }
                    throw ArcError("NASM backend: # only on arrays or str (got var '" + id->name + "')");
                }
                throw ArcError("NASM backend: # only on array/str variable");
            }
            emitExprTo(u->operand.get(), out, "rax");
            if (u->op == "-") out << "    neg rax\n";
            else if (u->op == "~") out << "    not rax\n";
            else if (u->op == "!") {
                out << "    test rax, rax\n";
                out << "    sete al\n";
                out << "    movzx rax, al\n";
            }
            else throw ArcError("NASM backend: unsupported unary op: " + u->op);
            if (dst != "rax") out << "    mov " << dst << ", rax\n";
            return;
        }
        if (auto* c = dynamic_cast<CallExpr*>(e)) {
            auto* id = dynamic_cast<IdentExpr*>(c->callee.get());
            if (!id)
                throw ArcError("NASM backend: only direct function calls supported");
            size_t n = c->args.size();
            int intRegCount = target_->intArgRegs();
            int fltRegCount = (target_->name() == "win-x86_64") ? 4 : 8;

            std::vector<bool> isFltArg(n, false);
            int ni = 0, nf = 0;
            for (size_t k = 0; k < n; ++k) {
                if (isFloatExpr(c->args[k].get())) { isFltArg[k] = true; ++nf; }
                else { ++ni; }
            }
            if (ni > intRegCount)
                throw ArcError("NASM backend: too many int args for call " + id->name);
            if (nf > fltRegCount)
                throw ArcError("NASM backend: too many flt args for call " + id->name);

            int shadow = target_->shadowSpace();
            int X = shadow;
            while ((X + static_cast<int>(n) * 8) % 16 != 0) X += 8;

            for (size_t k = 0; k < n; ++k) {
                bool isArrArg = false;
                if (auto* aid = dynamic_cast<IdentExpr*>(c->args[k].get())) {
                    auto it = locals_.find(aid->name);
                    if (it != locals_.end()) {
                        if (arrParamNames_.count(aid->name)) {
                            out << "    mov rax, [rbp " << it->second << "]\n";
                            isArrArg = true;
                        } else if (arrayN_.count(aid->name)) {
                            out << "    lea rax, [rbp " << it->second << "]\n";
                            isArrArg = true;
                        }
                    }
                }
                if (!isArrArg) {
                    if (isFltArg[k]) {
                        emitFloatToXmm0(c->args[k].get(), out);
                        out << "    movq rax, xmm0\n";
                    } else {
                        emitExprTo(c->args[k].get(), out, "rax");
                    }
                }
                out << "    push rax\n";
            }
            if (X > 0) out << "    sub rsp, " << X << "\n";

            int intIdx = 0, fltIdx2 = 0;
            for (size_t k = 0; k < n; ++k) {
                if (!isFltArg[k]) continue;
                std::string xmm = target_->floatArgReg(fltIdx2++);
                out << "    movsd " << xmm << ", [rsp + "
                    << X + (static_cast<int>(n) - 1 - k) * 8 << "]\n";
            }
            for (size_t k = 0; k < n; ++k) {
                if (isFltArg[k]) continue;
                std::string reg = target_->intArgReg(intIdx++);
                out << "    mov " << reg << ", [rsp + "
                    << X + (static_cast<int>(n) - 1 - k) * 8 << "]\n";
            }
            out << "    call arc_fn_" << id->name << "\n";
            int cleanup = static_cast<int>(n) * 8 + X;
            out << "    add rsp, " << cleanup << "\n";
            if (dst != "rax" && dst != "xmm0") out << "    mov " << dst << ", rax\n";
            return;
        }
        throw ArcError("NASM backend: unsupported expression");
    }

    void emitCondition(Expr* e, std::ostringstream& out, const std::string& falseLbl) {
        if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
            bool isStrCmp = false;
            if (b->op == "==" || b->op == "!=") {
                auto isStrE = [&](Expr* x) -> bool {
                    if (dynamic_cast<StrExpr*>(x)) return true;
                    if (auto* id2 = dynamic_cast<IdentExpr*>(x))
                        return strVars_.count(id2->name) > 0;
                    return false;
                };
                if (isStrE(b->lhs.get()) || isStrE(b->rhs.get()))
                    isStrCmp = true;
            }
            if (isStrCmp) {
                emitExprTo(e, out, "rax");
                out << "    test rax, rax\n";
                out << "    jz " << falseLbl << "\n";
                return;
            }
            std::string op = b->op;
            std::string negOp;
            if      (op == "==") negOp = "!=";
            else if (op == "!=") negOp = "==";
            else if (op == "<")  negOp = ">=";
            else if (op == ">")  negOp = "<=";
            else if (op == "<=") negOp = ">";
            else if (op == ">=") negOp = "<";
            if (!negOp.empty()) {
                emitExprTo(b->lhs.get(), out, "rax");
                out << "    push rax\n";
                emitExprTo(b->rhs.get(), out, "rax");
                out << "    mov rcx, rax\n";
                out << "    pop rax\n";
                out << "    cmp rax, rcx\n";
                if      (negOp == "==") out << "    je  " << falseLbl << "\n";
                else if (negOp == "!=") out << "    jne " << falseLbl << "\n";
                else if (negOp == "<")  out << "    jl  " << falseLbl << "\n";
                else if (negOp == ">")  out << "    jg  " << falseLbl << "\n";
                else if (negOp == "<=") out << "    jle " << falseLbl << "\n";
                else                    out << "    jge " << falseLbl << "\n";
                return;
            }
        }
        emitExprTo(e, out, "rax");
        out << "    test rax, rax\n";
        out << "    jz " << falseLbl << "\n";
    }

    void emitBody(const std::vector<StmtPtr>& stmts,
                  std::ostringstream& out,
                  std::ostringstream& rodata) {
        for (auto& s : stmts) {
            if (auto* vd = dynamic_cast<VarDeclStmt*>(s.get())) {
                for (auto& it : vd->vars) {
                    if (it.type && it.type->kind == Type::Str) {
                        nextLocalOffset_ -= 16;
                        locals_[it.name] = nextLocalOffset_;
                        strVars_.insert(it.name);
                        out << "    mov qword [rbp " << nextLocalOffset_ << "], 0\n";
                        out << "    mov qword [rbp " << (nextLocalOffset_ + 8) << "], 0\n";
                        continue;
                    }
                    int slots = 1;
                    if (it.type && it.type->kind == Type::Arr)
                        slots = static_cast<int>(it.type->arraySize);
                    else if (it.type && it.type->kind == Type::Flt)
                        fltVars_.insert(it.name);
                    else if (it.type && it.type->kind != Type::Int)
                        throw ArcError("NASM backend: unsupported var type");
                    nextLocalOffset_ -= slots * 8;
                    locals_[it.name] = nextLocalOffset_;
                    if (slots > 1) arrayN_[it.name] = slots;
                }
                continue;
            }
            if (auto* es = dynamic_cast<ExprStmt*>(s.get())) {
                if (dynamic_cast<CallExpr*>(es->expr.get())) {
                    emitExprTo(es->expr.get(), out, "rax");
                    continue;
                }
                throw ArcError("NASM backend: unsupported expr stmt");
            }
            if (auto* as = dynamic_cast<AssignStmt*>(s.get())) {
                if (auto* id = dynamic_cast<IdentExpr*>(as->target.get())) {
                    if (strVars_.count(id->name)) {
                        auto it = locals_.find(id->name);
                        if (it == locals_.end())
                            throw ArcError("NASM backend: str var not found");
                        if (auto* se = dynamic_cast<StrExpr*>(as->value.get())) {
                            int rid = rodataCounter_++;
                            std::string label = "str" + std::to_string(rid);
                            rodata << label << ": db ";
                            for (unsigned char c : se->v) {
                                if (c == '"') rodata << "34, ";
                                else if (c == '\\') rodata << "92, ";
                                else if (c >= 32 && c < 127) rodata << "'" << c << "', ";
                                else rodata << static_cast<int>(c) << ", ";
                            }
                            rodata << "0\n";
                            out << "    lea rax, [rel " << label << "]\n";
                            out << "    mov [rbp " << it->second << "], rax\n";
                            out << "    mov qword [rbp " << (it->second + 8)
                                << "], " << se->v.size() << "\n";
                            continue;
                        }
                        if (auto* srcId = dynamic_cast<IdentExpr*>(as->value.get())) {
                            auto sit = locals_.find(srcId->name);
                            if (sit == locals_.end() || !strVars_.count(srcId->name))
                                throw ArcError("NASM backend: str = non-str");
                            out << "    mov rax, [rbp " << sit->second << "]\n";
                            out << "    mov [rbp " << it->second << "], rax\n";
                            out << "    mov rax, [rbp " << (sit->second + 8) << "]\n";
                            out << "    mov [rbp " << (it->second + 8) << "], rax\n";
                            continue;
                        }
                        if (auto* bin = dynamic_cast<BinaryExpr*>(as->value.get())) {
                            if (bin->op != "+")
                                throw ArcError("NASM backend: str only supports +");
                            auto loadStr = [&](Expr* e, const std::string& ptrReg,
                                               const std::string& lenReg) {
                                if (auto* se2 = dynamic_cast<StrExpr*>(e)) {
                                    int rid = rodataCounter_++;
                                    std::string lbl = "str" + std::to_string(rid);
                                    rodata << lbl << ": db ";
                                    for (unsigned char c : se2->v) {
                                        if (c == '"') rodata << "34, ";
                                        else if (c == '\\') rodata << "92, ";
                                        else if (c >= 32 && c < 127) rodata << "'" << c << "', ";
                                        else rodata << static_cast<int>(c) << ", ";
                                    }
                                    rodata << "0\n";
                                    out << "    lea " << ptrReg << ", [rel " << lbl << "]\n";
                                    out << "    mov " << lenReg << ", " << se2->v.size() << "\n";
                                    return;
                                }
                                if (auto* id2 = dynamic_cast<IdentExpr*>(e)) {
                                    auto sit2 = locals_.find(id2->name);
                                    if (sit2 == locals_.end() || !strVars_.count(id2->name))
                                        throw ArcError("NASM backend: str+ needs str operands");
                                    out << "    mov " << ptrReg << ", [rbp " << sit2->second << "]\n";
                                    out << "    mov " << lenReg << ", [rbp " << (sit2->second + 8) << "]\n";
                                    return;
                                }
                                throw ArcError("NASM backend: str+ needs str operands");
                            };
                            loadStr(bin->lhs.get(), "rdi", "rsi");
                            loadStr(bin->rhs.get(), "rdx", "rcx");
                            out << "    call __arc_str_concat\n";
                            out << "    mov [rbp " << it->second << "], rax\n";
                            out << "    mov [rbp " << (it->second + 8) << "], rdx\n";
                            continue;
                        }
                        if (auto* fe = dynamic_cast<FmtExpr*>(as->value.get())) {
                            nextLocalOffset_ -= 16;
                            int accPtrOff = nextLocalOffset_;
                            int accLenOff = nextLocalOffset_ + 8;
                            out << "    lea rax, [rel __arc_empty_str]\n";
                            out << "    mov [rbp " << accPtrOff << "], rax\n";
                            out << "    mov qword [rbp " << accLenOff << "], 0\n";
                            for (auto& part : fe->parts) {
                                out << "    mov rdi, [rbp " << accPtrOff << "]\n";
                                out << "    mov rsi, [rbp " << accLenOff << "]\n";
                                if (part.isExpr) {
                                    emitStrPartToRegs(part.expr.get(), out, rodata, "rdx", "rcx");
                                } else {
                                    int rid = rodataCounter_++;
                                    std::string lbl = "str" + std::to_string(rid);
                                    rodata << lbl << ": db ";
                                    for (unsigned char c : part.text) {
                                        if (c == '"') rodata << "34, ";
                                        else if (c == '\\') rodata << "92, ";
                                        else if (c >= 32 && c < 127) rodata << "'" << c << "', ";
                                        else rodata << static_cast<int>(c) << ", ";
                                    }
                                    rodata << "0\n";
                                    out << "    lea rdx, [rel " << lbl << "]\n";
                                    out << "    mov rcx, " << part.text.size() << "\n";
                                }
                                out << "    call __arc_str_concat\n";
                                out << "    mov [rbp " << accPtrOff << "], rax\n";
                                out << "    mov [rbp " << accLenOff << "], rdx\n";
                            }
                            out << "    mov rax, [rbp " << accPtrOff << "]\n";
                            out << "    mov [rbp " << it->second << "], rax\n";
                            out << "    mov rax, [rbp " << accLenOff << "]\n";
                            out << "    mov [rbp " << (it->second + 8) << "], rax\n";
                            continue;
                        }
                        throw ArcError("NASM backend: str only from literal / str var / str+str");
                    }
                }
                if (auto* al = dynamic_cast<ArrayLitExpr*>(as->value.get())) {
                    auto* id = dynamic_cast<IdentExpr*>(as->target.get());
                    if (!id)
                        throw ArcError("NASM backend: array literal only assignable to array var");
                    auto it = locals_.find(id->name);
                    auto nit = arrayN_.find(id->name);
                    if (it == locals_.end() || nit == arrayN_.end())
                        throw ArcError("NASM backend: not an array: " + id->name);
                    if (static_cast<int64_t>(al->elems.size()) > nit->second)
                        throw ArcError("NASM backend: too many elements for " + id->name);
                    for (size_t k = 0; k < al->elems.size(); ++k) {
                        auto* ie = dynamic_cast<IntExpr*>(al->elems[k].get());
                        if (!ie)
                            throw ArcError("NASM backend: array literal only int literals for now");
                        out << "    mov rax, " << ie->v << "\n";
                        out << "    mov [rbp " << (it->second + static_cast<int>(k) * 8)
                            << "], rax\n";
                    }
                    continue;
                }
                if (auto* ix = dynamic_cast<IndexExpr*>(as->target.get())) {
                    auto* cid = dynamic_cast<IdentExpr*>(ix->container.get());
                    if (!cid)
                        throw ArcError("NASM backend: index assign only on array var");
                    auto it = locals_.find(cid->name);
                    if (it == locals_.end() || !arrayN_.count(cid->name))
                        throw ArcError("NASM backend: not an array: " + cid->name);
                    emitExprTo(ix->index.get(), out, "rax");
                    out << "    push rax\n";
                    emitExprTo(as->value.get(), out, "rax");
                    out << "    mov rcx, rax\n";
                    out << "    pop rax\n";
                    out << "    lea rdx, [rbp " << it->second << "]\n";
                    out << "    mov [rdx + rax * 8], rcx\n";
                    continue;
                }
                auto* id = dynamic_cast<IdentExpr*>(as->target.get());
                if (!id)
                    throw ArcError("NASM backend: only simple assignments");
                emitExprTo(as->value.get(), out, "rax");
                auto it = locals_.find(id->name);
                if (it != locals_.end()) {
                    out << "    mov [rbp " << it->second << "], rax\n";
                } else if (globalNames_.count(id->name)) {
                    out << "    mov [rel g_" << id->name << "], rax\n";
                } else {
                    throw ArcError("NASM backend: undefined variable: " + id->name);
                }
                continue;
            }
            if (auto* p = dynamic_cast<PrintStmt*>(s.get())) {
                for (size_t ai = 0; ai < p->exprs.size(); ++ai) {
                    auto* arg = p->exprs[ai].get();
                    bool printNewline = (ai == p->exprs.size() - 1) && p->newline;

                    if (isFloatExpr(arg)) {
                        emitFloatToXmm0(arg, out);
                        if (target_->name() == "win-x86_64") {
                            out << "    lea rcx, [rel __arc_fmt_flt]\n";
                            out << "    movq xmm1, xmm0\n";
                            out << "    sub rsp, 32\n";
                            out << "    call printf\n";
                            out << "    add rsp, 32\n";
                            if (printNewline) {
                                out << "    lea rcx, [rel __arc_nl]\n";
                                out << "    sub rsp, 32\n";
                                out << "    call printf\n";
                                out << "    add rsp, 32\n";
                            }
                        } else {
                            out << "    call __arc_put_flt\n";
                            if (printNewline) out << "    call __arc_put_nl\n";
                        }
                        continue;
                    }

                    if (auto* fe = dynamic_cast<FmtExpr*>(arg)) {
                        for (size_t pi = 0; pi < fe->parts.size(); ++pi) {
                            auto& part = fe->parts[pi];
                            bool isLast = (pi == fe->parts.size() - 1)
                                          && (ai == p->exprs.size() - 1)
                                          && p->newline;
                            if (!part.isExpr) {
                                if (part.text.empty()) continue;
                                int rid = rodataCounter_++;
                                std::string lbl = "str" + std::to_string(rid);
                                rodata << lbl << ": db ";
                                for (unsigned char c : part.text) {
                                    if (c == '"') rodata << "34, ";
                                    else if (c == '\\') rodata << "92, ";
                                    else if (c >= 32 && c < 127) rodata << "'" << c << "', ";
                                    else rodata << static_cast<int>(c) << ", ";
                                }
                                if (isLast) rodata << "10, ";
                                rodata << "0\n";
                                size_t len = part.text.size() + (isLast ? 1 : 0);
                                if (target_->name() == "linux-x86_64") {
                                    out << "    lea rdi, [rel " << lbl << "]\n";
                                    out << "    mov rsi, " << len << "\n";
                                    out << "    call __arc_put_str\n";
                                } else {
                                    out << "    lea rcx, [rel __arc_fmt_str]\n";
                                    out << "    lea rdx, [rel " << lbl << "]\n";
                                    out << "    sub rsp, 32\n";
                                    out << "    call printf\n";
                                    out << "    add rsp, 32\n";
                                }
                            } else {
                                auto* sub = part.expr.get();
                                if (auto* id2 = dynamic_cast<IdentExpr*>(sub)) {
                                    if (strVars_.count(id2->name)) {
                                        auto sit = locals_.find(id2->name);
                                        if (sit == locals_.end())
                                            throw ArcError("NASM backend: fmt str var not found");
                                        int off = sit->second;
                                        if (target_->name() == "linux-x86_64") {
                                            out << "    mov rdi, [rbp " << off << "]\n";
                                            out << "    mov rsi, [rbp " << (off + 8) << "]\n";
                                            out << "    call __arc_put_str\n";
                                            if (isLast) out << "    call __arc_put_nl\n";
                                        } else {
                                            out << "    lea rcx, [rel __arc_fmt_strn]\n";
                                            out << "    mov edx, [rbp " << (off + 8) << "]\n";
                                            out << "    mov r8, [rbp " << off << "]\n";
                                            out << "    sub rsp, 32\n";
                                            out << "    call printf\n";
                                            out << "    add rsp, 32\n";
                                            if (isLast) {
                                                out << "    lea rcx, [rel __arc_nl]\n";
                                                out << "    sub rsp, 32\n";
                                                out << "    call printf\n";
                                                out << "    add rsp, 32\n";
                                            }
                                        }
                                        continue;
                                    }
                                }
                                emitExprTo(sub, out, "rax");
                                if (target_->name() == "linux-x86_64") {
                                    out << "    mov rdi, rax\n";
                                    out << "    call __arc_put_int\n";
                                    if (isLast) out << "    call __arc_put_nl\n";
                                } else {
                                    out << "    mov rdx, rax\n";
                                    out << "    lea rcx, [rel __arc_fmt_int]\n";
                                    out << "    sub rsp, 32\n";
                                    out << "    call printf\n";
                                    out << "    add rsp, 32\n";
                                    if (isLast) {
                                        out << "    lea rcx, [rel __arc_nl]\n";
                                        out << "    sub rsp, 32\n";
                                        out << "    call printf\n";
                                        out << "    add rsp, 32\n";
                                    }
                                }
                            }
                        }
                        continue;
                    }
                if (auto* id = dynamic_cast<IdentExpr*>(arg)) {
                    auto nit = arrayN_.find(id->name);
                    if (nit != arrayN_.end()) {
                        auto it = locals_.find(id->name);
                        if (it == locals_.end())
                            throw ArcError("NASM backend: array var not found: " + id->name);
                        int baseOff = it->second;
                        int64_t N = nit->second;

                        if (target_->name() == "win-x86_64") {
                            out << "    lea rcx, [rel __arc_fmt_str]\n";
                            out << "    lea rdx, [rel __arc_brk_open]\n";
                            out << "    sub rsp, 32\n";
                            out << "    call printf\n";
                            out << "    add rsp, 32\n";
                            for (int64_t k = 0; k < N; ++k) {
                                if (k > 0) {
                                    out << "    lea rcx, [rel __arc_fmt_str]\n";
                                    out << "    lea rdx, [rel __arc_brk_comma]\n";
                                    out << "    sub rsp, 32\n";
                                    out << "    call printf\n";
                                    out << "    add rsp, 32\n";
                                }
                                out << "    mov rdx, [rbp " << (baseOff + static_cast<int>(k) * 8) << "]\n";
                                out << "    lea rcx, [rel __arc_fmt_int]\n";
                                out << "    sub rsp, 32\n";
                                out << "    call printf\n";
                                out << "    add rsp, 32\n";
                            }
                            if (printNewline) {
                                out << "    lea rcx, [rel __arc_fmt_str]\n";
                                out << "    lea rdx, [rel __arc_brk_close_nl]\n";
                                out << "    sub rsp, 32\n";
                                out << "    call printf\n";
                                out << "    add rsp, 32\n";
                            } else {
                                out << "    lea rcx, [rel __arc_fmt_str]\n";
                                out << "    lea rdx, [rel __arc_brk_close]\n";
                                out << "    sub rsp, 32\n";
                                out << "    call printf\n";
                                out << "    add rsp, 32\n";
                            }
                        } else {
                            throw ArcError("NASM backend: prn array only on Windows for now");
                        }
                        continue;
                    }
                }
                if (auto* id = dynamic_cast<IdentExpr*>(arg)) {
                    if (strVars_.count(id->name)) {
                        auto it = locals_.find(id->name);
                        if (it == locals_.end())
                            throw ArcError("NASM backend: str var not found");
                        int off = it->second;
                        if (target_->name() == "win-x86_64") {
                            out << "    lea rcx, [rel __arc_fmt_strn]\n";
                            out << "    mov edx, [rbp " << (off + 8) << "]\n";
                            out << "    mov r8, [rbp " << off << "]\n";
                            out << "    sub rsp, 32\n";
                            out << "    call printf\n";
                            out << "    add rsp, 32\n";
                            if (printNewline) {
                                out << "    lea rcx, [rel __arc_nl]\n";
                                out << "    sub rsp, 32\n";
                                out << "    call printf\n";
                                out << "    add rsp, 32\n";
                            }
                        } else {
                            out << "    mov rdi, [rbp " << off << "]\n";
                            out << "    mov rsi, [rbp " << (off + 8) << "]\n";
                            out << "    call __arc_put_str\n";
                            if (printNewline) out << "    call __arc_put_nl\n";
                        }
                        continue;
                    }
                }
                if (auto* se = dynamic_cast<StrExpr*>(arg)) {
                    int id = rodataCounter_++;
                    std::string label = "str" + std::to_string(id);
                    rodata << label << ": db ";
                    for (unsigned char c : se->v) {
                        if (c == '"') rodata << "34, ";
                        else if (c == '\\') rodata << "92, ";
                        else if (c >= 32 && c < 127) rodata << "'" << c << "', ";
                        else rodata << static_cast<int>(c) << ", ";
                    }
                    if (printNewline) rodata << "10, ";
                    rodata << "0\n";

                    if (target_->name() == "linux-x86_64") {
                        out << "    lea rdi, [rel " << label << "]\n";
                        out << "    mov rsi, " << (se->v.size() + (printNewline ? 1 : 0)) << "\n";
                        out << "    call __arc_put_str\n";
                    } else {
                        out << "    lea rcx, [rel __arc_fmt_str]\n";
                        out << "    lea rdx, [rel " << label << "]\n";
                        out << "    sub rsp, 32\n";
                        out << "    call printf\n";
                        out << "    add rsp, 32\n";
                    }
                } else if (dynamic_cast<IntExpr*>(arg) ||
                           dynamic_cast<IdentExpr*>(arg) ||
                           dynamic_cast<BinaryExpr*>(arg) ||
                           dynamic_cast<CallExpr*>(arg) ||
                           dynamic_cast<UnaryExpr*>(arg) ||
                           dynamic_cast<IndexExpr*>(arg) ||
                           dynamic_cast<MemberExpr*>(arg)) {
                    bool isBoolExpr = false;
                    if (auto* b2 = dynamic_cast<BinaryExpr*>(arg)) {
                        const std::string& o2 = b2->op;
                        if (o2 == "==" || o2 == "!=" || o2 == "<" || o2 == ">" ||
                            o2 == "<=" || o2 == ">=" || o2 == "&&" || o2 == "||")
                            isBoolExpr = true;
                    } else if (auto* u2 = dynamic_cast<UnaryExpr*>(arg)) {
                        if (u2->op == "!") isBoolExpr = true;
                    }
                    emitExprTo(arg, out, "rax");
                    if (isBoolExpr) {
                        int bid = labelCounter_++;
                        std::string truLbl = ".prn_tru_" + std::to_string(bid);
                        std::string doneLbl = ".prn_done_" + std::to_string(bid);
                        out << "    test rax, rax\n";
                        out << "    jnz " << truLbl << "\n";
                        if (target_->name() == "linux-x86_64") {
                            out << "    lea rdi, [rel __arc_str_fal]\n";
                            out << "    mov rsi, 3\n";
                            out << "    call __arc_put_str\n";
                            if (printNewline) out << "    call __arc_put_nl\n";
                        } else {
                            out << "    lea rcx, [rel __arc_fmt_str]\n";
                            out << "    lea rdx, [rel __arc_str_fal]\n";
                            out << "    sub rsp, 32\n";
                            out << "    call printf\n";
                            out << "    add rsp, 32\n";
                            if (printNewline) {
                                out << "    lea rcx, [rel __arc_nl]\n";
                                out << "    sub rsp, 32\n";
                                out << "    call printf\n";
                                out << "    add rsp, 32\n";
                            }
                        }
                        out << "    jmp " << doneLbl << "\n";
                        out << truLbl << ":\n";
                        if (target_->name() == "linux-x86_64") {
                            out << "    lea rdi, [rel __arc_str_tru]\n";
                            out << "    mov rsi, 3\n";
                            out << "    call __arc_put_str\n";
                            if (printNewline) out << "    call __arc_put_nl\n";
                        } else {
                            out << "    lea rcx, [rel __arc_fmt_str]\n";
                            out << "    lea rdx, [rel __arc_str_tru]\n";
                            out << "    sub rsp, 32\n";
                            out << "    call printf\n";
                            out << "    add rsp, 32\n";
                            if (printNewline) {
                                out << "    lea rcx, [rel __arc_nl]\n";
                                out << "    sub rsp, 32\n";
                                out << "    call printf\n";
                                out << "    add rsp, 32\n";
                            }
                        }
                        out << doneLbl << ":\n";
                    } else {
                        if (target_->name() == "linux-x86_64") {
                            out << "    mov rdi, rax\n";
                            out << "    call __arc_put_int\n";
                            if (printNewline) out << "    call __arc_put_nl\n";
                        } else {
                            out << "    mov rdx, rax\n";
                            out << "    lea rcx, [rel __arc_fmt_int]\n";
                            out << "    sub rsp, 32\n";
                            out << "    call printf\n";
                            out << "    add rsp, 32\n";
                            if (printNewline) {
                                out << "    lea rcx, [rel __arc_nl]\n";
                                out << "    sub rsp, 32\n";
                                out << "    call printf\n";
                                out << "    add rsp, 32\n";
                            }
                        }
                    }
                } else {
                    throw ArcError("NASM backend: unsupported print expr");
                }
                }
                continue;
            }
            if (auto* i = dynamic_cast<IfStmt*>(s.get())) {
                int id = labelCounter_++;
                std::string elseLbl = ".if_else_" + std::to_string(id);
                std::string endLbl  = ".if_end_"  + std::to_string(id);
                emitCondition(i->cond.get(), out, elseLbl);
                emitBody(i->thenBody, out, rodata);
                if (!i->elseBody.empty()) {
                    out << "    jmp " << endLbl << "\n";
                }
                out << elseLbl << ":\n";
                if (!i->elseBody.empty()) {
                    emitBody(i->elseBody, out, rodata);
                    out << endLbl << ":\n";
                }
                continue;
            }
            if (auto* w = dynamic_cast<WhileStmt*>(s.get())) {
                int id = labelCounter_++;
                std::string startLbl = ".whl_start_" + std::to_string(id);
                std::string endLbl   = ".whl_end_"   + std::to_string(id);
                out << startLbl << ":\n";
                emitCondition(w->cond.get(), out, endLbl);
                loopStack_.push_back({startLbl, endLbl, w->label, ""});
                emitBody(w->body, out, rodata);
                loopStack_.pop_back();
                out << "    jmp " << startLbl << "\n";
                out << endLbl << ":\n";
                continue;
            }
            if (auto* fr = dynamic_cast<ForRangeStmt*>(s.get())) {
                if (fr->isForEach)
                    throw ArcError("NASM backend: for-each not yet supported");
                int64_t stepVal = 1;
                if (fr->step) {
                    if (auto* sie = dynamic_cast<IntExpr*>(fr->step.get()))
                        stepVal = sie->v;
                    else
                        throw ArcError("NASM backend: for step must be int literal");
                    if (stepVal <= 0)
                        throw ArcError("NASM backend: for step must be positive");
                }
                if (!locals_.count(fr->varName)) {
                    nextLocalOffset_ -= 8;
                    locals_[fr->varName] = nextLocalOffset_;
                }
                int varOff = locals_[fr->varName];
                int id = labelCounter_++;
                std::string startLbl = ".for_start_" + std::to_string(id);
                std::string endLbl   = ".for_end_"   + std::to_string(id);
                emitExprTo(fr->from.get(), out, "rax");
                out << "    mov [rbp " << varOff << "], rax\n";
                out << startLbl << ":\n";
                out << "    mov rax, [rbp " << varOff << "]\n";
                out << "    push rax\n";
                emitExprTo(fr->to.get(), out, "rax");
                out << "    mov rcx, rax\n";
                out << "    pop rax\n";
                out << "    cmp rax, rcx\n";
                if (fr->exclusive) out << "    jge " << endLbl << "\n";
                else               out << "    jg "  << endLbl << "\n";
                loopStack_.push_back({startLbl, endLbl, fr->label, ""});
                emitBody(fr->body, out, rodata);
                loopStack_.pop_back();
                out << "    mov rax, [rbp " << varOff << "]\n";
                out << "    add rax, " << stepVal << "\n";
                out << "    mov [rbp " << varOff << "], rax\n";
                out << "    jmp " << startLbl << "\n";
                out << endLbl << ":\n";
                continue;
            }
            if (auto* fc = dynamic_cast<ForCStmt*>(s.get())) {
                auto emitForSimple = [&](Stmt* st) {
                    if (!st) return;
                    if (auto* as = dynamic_cast<AssignStmt*>(st)) {
                        auto* id2 = dynamic_cast<IdentExpr*>(as->target.get());
                        if (!id2)
                            throw ArcError("NASM backend: forC init/incr target must be ident");
                        auto it2 = locals_.find(id2->name);
                        if (it2 == locals_.end())
                            throw ArcError("NASM backend: forC undefined var: " + id2->name);
                        emitExprTo(as->value.get(), out, "rax");
                        out << "    mov [rbp " << it2->second << "], rax\n";
                        return;
                    }
                    if (auto* inc = dynamic_cast<IncDecStmt*>(st)) {
                        auto* id2 = dynamic_cast<IdentExpr*>(inc->target.get());
                        if (!id2)
                            throw ArcError("NASM backend: forC inc target must be ident");
                        auto it2 = locals_.find(id2->name);
                        if (it2 == locals_.end())
                            throw ArcError("NASM backend: forC undefined var: " + id2->name);
                        out << "    mov rax, [rbp " << it2->second << "]\n";
                        out << (inc->isInc ? "    add rax, 1\n" : "    sub rax, 1\n");
                        out << "    mov [rbp " << it2->second << "], rax\n";
                        return;
                    }
                    if (auto* es = dynamic_cast<ExprStmt*>(st)) {
                        emitExprTo(es->expr.get(), out, "rax");
                        return;
                    }
                    throw ArcError("NASM backend: unsupported forC init/incr");
                };

                if (fc->init && dynamic_cast<VarDeclStmt*>(fc->init.get()))
                    throw ArcError("NASM backend: forC init var decl unsupported; declare var before loop");

                int id2 = labelCounter_++;
                std::string startLbl = ".forc_start_" + std::to_string(id2);
                std::string incrLbl  = ".forc_incr_"  + std::to_string(id2);
                std::string endLbl   = ".forc_end_"   + std::to_string(id2);

                emitForSimple(fc->init.get());
                out << startLbl << ":\n";
                if (fc->cond) {
                    emitCondition(fc->cond.get(), out, endLbl);
                }
                loopStack_.push_back({startLbl, endLbl, fc->label, incrLbl});
                emitBody(fc->body, out, rodata);
                loopStack_.pop_back();
                out << incrLbl << ":\n";
                emitForSimple(fc->incr.get());
                out << "    jmp " << startLbl << "\n";
                out << endLbl << ":\n";
                continue;
            }

            if (auto* ls = dynamic_cast<LabelStmt*>(s.get())) {
                out << ".L_" << ls->name << ":\n";
                continue;
            }
            if (auto* g = dynamic_cast<GetStmt*>(s.get())) {
                for (auto& tgt : g->targets) {
                    auto* id = dynamic_cast<IdentExpr*>(tgt.get());
                    if (!id)
                        throw ArcError("NASM backend: get only supports simple variables");
                    auto it = locals_.find(id->name);
                    bool isGlobal = (it == locals_.end());
                    if (isGlobal && !globalNames_.count(id->name))
                        throw ArcError("NASM backend: get on undefined variable: " + id->name);

                    if (target_->name() == "win-x86_64") {
                        out << "    lea rcx, [rel __arc_fmt_scanint]\n";
                        if (isGlobal) out << "    lea rdx, [rel g_" << id->name << "]\n";
                        else          out << "    lea rdx, [rbp " << it->second << "]\n";
                        out << "    sub rsp, 32\n";
                        out << "    call scanf\n";
                        out << "    add rsp, 32\n";
                    } else {
                        out << "    call __arc_read_int\n";
                        if (isGlobal) out << "    mov [rel g_" << id->name << "], rax\n";
                        else          out << "    mov [rbp " << it->second << "], rax\n";
                    }
                }
                continue;
            }
            if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
                int id = labelCounter_++;
                std::string endLbl = ".swt_end_" + std::to_string(id);
                std::string defLbl = sw->hasDefault
                    ? (".swt_def_" + std::to_string(id)) : endLbl;
                nextLocalOffset_ -= 8;
                int tmpOff = nextLocalOffset_;
                locals_["__swt_tmp_" + std::to_string(id)] = tmpOff;

                emitExprTo(sw->subject.get(), out, "rax");
                out << "    mov [rbp " << tmpOff << "], rax\n";

                std::vector<std::string> caseLbls;
                for (size_t i = 0; i < sw->cases.size(); ++i) {
                    std::string cl = ".swt_case_" + std::to_string(id)
                                     + "_" + std::to_string(i);
                    caseLbls.push_back(cl);
                    auto& cc = sw->cases[i];
                    if (!cc.pattern)
                        throw ArcError("NASM backend: swt case missing pattern");
                    std::string cmpVal;
                    switch (cc.pattern->kind) {
                        case Pattern::LitInt:
                            cmpVal = std::to_string(cc.pattern->ival);
                            break;
                        case Pattern::LitChr:
                            cmpVal = std::to_string(static_cast<int>(
                                static_cast<unsigned char>(cc.pattern->cval)));
                            break;
                        case Pattern::LitBol:
                            cmpVal = cc.pattern->bval ? "1" : "0";
                            break;
                        case Pattern::LitNil:
                            cmpVal = "0";
                            break;
                        default:
                            throw ArcError("NASM backend: swt case unsupported pattern");
                    }
                    out << "    mov rax, [rbp " << tmpOff << "]\n";
                    out << "    cmp rax, " << cmpVal << "\n";
                    out << "    je " << cl << "\n";
                }
                out << "    jmp " << defLbl << "\n";

                for (size_t i = 0; i < sw->cases.size(); ++i) {
                    out << caseLbls[i] << ":\n";
                    emitBody(sw->cases[i].body, out, rodata);
                    out << "    jmp " << endLbl << "\n";
                }

                if (sw->hasDefault) {
                    out << defLbl << ":\n";
                    emitBody(sw->defBody, out, rodata);
                }
                out << endLbl << ":\n";
                continue;
            }
            if (auto* b = dynamic_cast<BreakStmt*>(s.get())) {
                if (loopStack_.empty())
                    throw ArcError("NASM backend: brk outside loop");
                std::string tgt = b->label;
                bool found = false;
                for (auto it = loopStack_.rbegin(); it != loopStack_.rend(); ++it) {
                    if (tgt.empty() || it->label == tgt) {
                        out << "    jmp " << it->endLbl << "\n";
                        found = true;
                        break;
                    }
                }
                if (!found)
                    throw ArcError("NASM backend: no loop labeled '" + tgt + "'");
                continue;
            }
            if (auto* c = dynamic_cast<ContStmt*>(s.get())) {
                if (loopStack_.empty())
                    throw ArcError("NASM backend: cnt outside loop");
                std::string tgt = c->label;
                bool found = false;
                for (auto it = loopStack_.rbegin(); it != loopStack_.rend(); ++it) {
                    if (tgt.empty() || it->label == tgt) {
                        const std::string& j = it->contLbl.empty()
                            ? it->startLbl : it->contLbl;
                        out << "    jmp " << j << "\n";
                        found = true;
                        break;
                    }
                }
                if (!found)
                    throw ArcError("NASM backend: no loop labeled '" + tgt + "'");
                continue;
            }
            if (auto* js = dynamic_cast<JumpStmt*>(s.get())) {
                out << "    jmp .L_" << js->label << "\n";
                continue;
            }

            if (auto* as = dynamic_cast<AsmStmt*>(s.get())) {
                std::string body = as->text;
                if (body.size() >= 2 && body.front() == '{' && body.back() == '}')
                    body = body.substr(1, body.size() - 2);
                out << "    ; --- inline asm (line " << as->line << ") ---\n";
                std::string line;
                auto flushLine = [&]() {
                    if (line.empty()) return;
                    std::string expanded;
                    size_t k = 0;
                    while (k < line.size()) {
                        if (line[k] == '%' && k + 1 < line.size() &&
                            (std::isalpha(static_cast<unsigned char>(line[k+1])) ||
                             line[k+1] == '_')) {
                            size_t j = k + 1;
                            while (j < line.size() &&
                                   (std::isalnum(static_cast<unsigned char>(line[j])) ||
                                    line[j] == '_')) ++j;
                            std::string name = line.substr(k + 1, j - k - 1);
                            auto it2 = locals_.find(name);
                            if (it2 != locals_.end()) {
                                expanded += "[rbp " + std::to_string(it2->second) + "]";
                            } else {
                                expanded += "%" + name;
                            }
                            k = j;
                        } else {
                            expanded += line[k++];
                        }
                    }
                    out << expanded << "\n";
                    line.clear();
                };
                for (size_t i = 0; i < body.size(); ++i) {
                    if (body[i] == '\n') flushLine();
                    else line += body[i];
                }
                flushLine();
                out << "    ; --- end inline asm ---\n";
                continue;
            }
            if (auto* r = dynamic_cast<RetStmt*>(s.get())) {
                if (r->value) {
                    if (isFloatExpr(r->value.get())) {
                        emitFloatToXmm0(r->value.get(), out);
                        out << "    movq rax, xmm0\n";
                    } else if (auto* ie = dynamic_cast<IntExpr*>(r->value.get())) {
                        out << "    mov eax, " << ie->v << "\n";
                    } else {
                        emitExprTo(r->value.get(), out, "rax");
                    }
                } else {
                    out << "    xor eax, eax\n";
                }
                out << "    mov r11, [rbp - " << frameSize_ << "]\n";
                out << "    mov [rel __arc_strbuf_used], r11\n";
                out << "    leave\n";
                out << "    ret\n";
                continue;
            }
            throw ArcError("NASM backend: statement kind not supported yet");
        }
    }

    void emitStrRuntime(std::ostringstream& out) {
        out << "__arc_str_copy:\n";
        out << "    test rcx, rcx\n";
        out << "    jz .scopy_done\n";
        out << ".scopy_loop:\n";
        out << "    mov al, [rsi]\n";
        out << "    mov [rdi], al\n";
        out << "    inc rsi\n";
        out << "    inc rdi\n";
        out << "    dec rcx\n";
        out << "    jnz .scopy_loop\n";
        out << ".scopy_done:\n";
        out << "    ret\n\n";

        out << "__arc_str_concat:\n";
        out << "    push rbx\n";
        out << "    push r12\n";
        out << "    push r13\n";
        out << "    push r14\n";
        out << "    push r15\n";
        out << "    mov r12, rsi\n";
        out << "    mov r13, rdx\n";
        out << "    mov r14, rcx\n";
        out << "    mov r15, rdi\n";
        out << "    mov rbx, r12\n";
        out << "    add rbx, r14\n";
        out << "    mov rax, [rel __arc_strbuf_used]\n";
        out << "    mov rcx, rax\n";
        out << "    add rcx, rbx\n";
        out << "    cmp rcx, 1048576\n";
        out << "    ja .sconcat_overflow\n";
        out << "    mov [rel __arc_strbuf_used], rcx\n";
        out << "    lea rcx, [rel __arc_strbuf]\n";
        out << "    add rax, rcx\n";
        out << "    mov r10, rax\n";
        out << "    mov rdi, r10\n";
        out << "    mov rsi, r15\n";
        out << "    mov rcx, r12\n";
        out << "    call __arc_str_copy\n";
        out << "    lea rdi, [r10 + r12]\n";
        out << "    mov rsi, r13\n";
        out << "    mov rcx, r14\n";
        out << "    call __arc_str_copy\n";
        out << "    mov rax, r10\n";
        out << "    mov rdx, rbx\n";
        out << "    pop r15\n";
        out << "    pop r14\n";
        out << "    pop r13\n";
        out << "    pop r12\n";
        out << "    pop rbx\n";
        out << "    ret\n";
        out << ".sconcat_overflow:\n";
        out << "    pop r15\n";
        out << "    pop r14\n";
        out << "    pop r13\n";
        out << "    pop r12\n";
        out << "    pop rbx\n";
        out << "    call __arc_strbuf_overflow\n";
        out << "    xor eax, eax\n";
        out << "    xor edx, edx\n";
        out << "    ret\n\n";

        out << "__arc_str_eq:\n";
        out << "    cmp rsi, rcx\n";
        out << "    jne .seq_no\n";
        out << "    test rsi, rsi\n";
        out << "    jz .seq_yes\n";
        out << ".seq_loop:\n";
        out << "    mov al, [rdi]\n";
        out << "    cmp al, [rdx]\n";
        out << "    jne .seq_no\n";
        out << "    inc rdi\n";
        out << "    inc rdx\n";
        out << "    dec rsi\n";
        out << "    jnz .seq_loop\n";
        out << ".seq_yes:\n";
        out << "    mov rax, 1\n";
        out << "    ret\n";
        out << ".seq_no:\n";
        out << "    xor eax, eax\n";
        out << "    ret\n\n";

        out << "__arc_strbuf_save:\n";
        out << "    mov rax, [rel __arc_strbuf_used]\n";
        out << "    ret\n\n";

        out << "__arc_put_flt_digits:\n";
        out << "    push rbp\n";
        out << "    mov rbp, rsp\n";
        out << "    sub rsp, 16\n";
        out << "    mov rax, rdi\n";
        out << "    mov r8, 10\n";
        out << "    lea rcx, [rbp - 1]\n";
        out << "    mov r9, 6\n";
        out << ".pfd_loop:\n";
        out << "    xor rdx, rdx\n";
        out << "    div r8\n";
        out << "    add dl, '0'\n";
        out << "    mov [rcx], dl\n";
        out << "    dec rcx\n";
        out << "    dec r9\n";
        out << "    jnz .pfd_loop\n";
        out << "    lea rsi, [rbp - 6]\n";
        out << "    mov rdi, 1\n";
        out << "    mov rdx, 6\n";
        out << "    mov rax, 1\n";
        out << "    syscall\n";
        out << "    leave\n";
        out << "    ret\n\n";

        out << "__arc_put_flt:\n";
        out << "    push rbp\n";
        out << "    mov rbp, rsp\n";
        out << "    sub rsp, 16\n";
        out << "    movq rax, xmm0\n";
        out << "    mov rcx, 0x8000000000000000\n";
        out << "    test rax, rcx\n";
        out << "    jz .pf_pos\n";
        out << "    mov rax, 1\n";
        out << "    mov rdi, 1\n";
        out << "    lea rsi, [rel __arc_minus]\n";
        out << "    mov rdx, 1\n";
        out << "    syscall\n";
        out << "    movq rax, xmm0\n";
        out << "    mov rcx, 0x8000000000000000\n";
        out << "    xor rax, rcx\n";
        out << "    movq xmm0, rax\n";
        out << ".pf_pos:\n";
        out << "    cvttsd2si rdi, xmm0\n";
        out << "    movsd [rbp - 8], xmm0\n";
        out << "    call __arc_put_int\n";
        out << "    mov rax, 1\n";
        out << "    mov rdi, 1\n";
        out << "    lea rsi, [rel __arc_dot]\n";
        out << "    mov rdx, 1\n";
        out << "    syscall\n";
        out << "    movsd xmm0, [rbp - 8]\n";
        out << "    cvttsd2si rax, xmm0\n";
        out << "    cvtsi2sd xmm1, rax\n";
        out << "    subsd xmm0, xmm1\n";
        out << "    mov rax, 0x412e848000000000\n";
        out << "    movq xmm1, rax\n";
        out << "    mulsd xmm0, xmm1\n";
        out << "    cvttsd2si rdi, xmm0\n";
        out << "    call __arc_put_flt_digits\n";
        out << "    leave\n";
        out << "    ret\n\n";

        out << "__arc_strbuf_restore:\n";
        out << "    mov [rel __arc_strbuf_used], rdi\n";
        out << "    ret\n\n";

        out << "__arc_int_to_str:\n";
        out << "    push rbp\n";
        out << "    mov rbp, rsp\n";
        out << "    sub rsp, 32\n";
        out << "    mov rax, rdi\n";
        out << "    lea rcx, [rbp - 1]\n";
        out << "    mov r8, 10\n";
        out << "    xor r9d, r9d\n";
        out << "    test rax, rax\n";
        out << "    jns .its_pos\n";
        out << "    neg rax\n";
        out << "    mov r9d, 1\n";
        out << ".its_pos:\n";
        out << ".its_loop:\n";
        out << "    xor rdx, rdx\n";
        out << "    div r8\n";
        out << "    add dl, '0'\n";
        out << "    mov [rcx], dl\n";
        out << "    dec rcx\n";
        out << "    test rax, rax\n";
        out << "    jnz .its_loop\n";
        out << "    test r9d, r9d\n";
        out << "    jz .its_emit\n";
        out << "    mov byte [rcx], '-'\n";
        out << "    dec rcx\n";
        out << ".its_emit:\n";
        out << "    inc rcx\n";
        out << "    lea rdx, [rbp - 1]\n";
        out << "    sub rdx, rcx\n";
        out << "    add rdx, 1\n";
        out << "    mov rsi, rcx\n";
        out << "    lea rdi, [rel __arc_its_buf]\n";
        out << "    mov rcx, rdx\n";
        out << "    rep movsb\n";
        out << "    lea rax, [rel __arc_its_buf]\n";
        out << "    leave\n";
        out << "    ret\n\n";

        if (target_->name() == "linux-x86_64") {
            out << "__arc_strbuf_overflow:\n";
            out << "    mov rax, 1\n";
            out << "    mov rdi, 2\n";
            out << "    lea rsi, [rel __arc_errbuf_over]\n";
            out << "    mov rdx, __arc_errbuf_over_len\n";
            out << "    syscall\n";
            out << "    mov rax, 60\n";
            out << "    mov rdi, 1\n";
            out << "    syscall\n\n";
        } else {
            out << "__arc_strbuf_overflow:\n";
            out << "    lea rcx, [rel __arc_fmt_str]\n";
            out << "    lea rdx, [rel __arc_errbuf_over]\n";
            out << "    sub rsp, 32\n";
            out << "    call printf\n";
            out << "    add rsp, 32\n";
            out << "    mov ecx, 1\n";
            out << "    call exit\n\n";
        }
    }

    void emitLinuxRuntime(std::ostringstream& out) {
        out << "__arc_put_str:\n";
        out << "    mov rax, 1\n";
        out << "    mov rdx, rsi\n";
        out << "    mov rsi, rdi\n";
        out << "    mov rdi, 1\n";
        out << "    syscall\n";
        out << "    ret\n\n";

        out << "__arc_put_nl:\n";
        out << "    mov rax, 1\n";
        out << "    mov rdi, 1\n";
        out << "    lea rsi, [rel __arc_nl_char]\n";
        out << "    mov rdx, 1\n";
        out << "    syscall\n";
        out << "    ret\n\n";

        out << "__arc_put_int:\n";
        out << "    push rbp\n";
        out << "    mov rbp, rsp\n";
        out << "    sub rsp, 32\n";
        out << "    lea rcx, [rbp - 1]\n";
        out << "    mov rax, rdi\n";
        out << "    mov r8, 10\n";
        out << "    xor r9d, r9d\n";
        out << "    test rax, rax\n";
        out << "    jns .itoa_pos\n";
        out << "    neg rax\n";
        out << "    mov r9d, 1\n";
        out << ".itoa_pos:\n";
        out << ".itoa_loop:\n";
        out << "    xor rdx, rdx\n";
        out << "    div r8\n";
        out << "    add dl, '0'\n";
        out << "    mov [rcx], dl\n";
        out << "    dec rcx\n";
        out << "    test rax, rax\n";
        out << "    jnz .itoa_loop\n";
        out << "    test r9d, r9d\n";
        out << "    jz .itoa_emit\n";
        out << "    mov byte [rcx], '-'\n";
        out << "    dec rcx\n";
        out << ".itoa_emit:\n";
        out << "    inc rcx\n";
        out << "    lea rdx, [rbp - 1]\n";
        out << "    sub rdx, rcx\n";
        out << "    add rdx, 1\n";
        out << "    mov rsi, rcx\n";
        out << "    mov rdi, 1\n";
        out << "    mov rax, 1\n";
        out << "    syscall\n";
        out << "    leave\n";
        out << "    ret\n\n";

        out << "__arc_read_int:\n";
        out << "    sub rsp, 80\n";
        out << "    mov rax, 0\n";
        out << "    xor rdi, rdi\n";
        out << "    mov rsi, rsp\n";
        out << "    mov rdx, 64\n";
        out << "    syscall\n";
        out << "    mov r10, rax\n";
        out << "    xor r8, r8\n";
        out << "    xor r9, r9\n";
        out << "    xor rcx, rcx\n";
        out << ".rdi_skip:\n";
        out << "    cmp rcx, r10\n";
        out << "    jge .rdi_done\n";
        out << "    movzx edx, byte [rsp + rcx]\n";
        out << "    cmp dl, ' '\n";
        out << "    je .rdi_adv\n";
        out << "    cmp dl, 9\n";
        out << "    je .rdi_adv\n";
        out << "    cmp dl, 10\n";
        out << "    je .rdi_adv\n";
        out << "    cmp dl, 13\n";
        out << "    je .rdi_adv\n";
        out << "    jmp .rdi_sign\n";
        out << ".rdi_adv:\n";
        out << "    inc rcx\n";
        out << "    jmp .rdi_skip\n";
        out << ".rdi_sign:\n";
        out << "    cmp dl, '-'\n";
        out << "    jne .rdi_loop\n";
        out << "    mov r9, 1\n";
        out << "    inc rcx\n";
        out << ".rdi_loop:\n";
        out << "    cmp rcx, r10\n";
        out << "    jge .rdi_finish\n";
        out << "    movzx edx, byte [rsp + rcx]\n";
        out << "    cmp dl, '0'\n";
        out << "    jl .rdi_finish\n";
        out << "    cmp dl, '9'\n";
        out << "    jg .rdi_finish\n";
        out << "    imul r8, r8, 10\n";
        out << "    sub dl, '0'\n";
        out << "    movzx edx, dl\n";
        out << "    add r8, rdx\n";
        out << "    inc rcx\n";
        out << "    jmp .rdi_loop\n";
        out << ".rdi_finish:\n";
        out << "    test r9, r9\n";
        out << "    jz .rdi_done\n";
        out << "    neg r8\n";
        out << ".rdi_done:\n";
        out << "    mov rax, r8\n";
        out << "    add rsp, 80\n";
        out << "    ret\n\n";
    }
};

} // namespace arc

// ============================================================================
// main 驱动
// ============================================================================
static std::string detectHostTarget() {
#ifdef _WIN32
    return "win-x86_64";
#else
    return "linux-x86_64";
#endif
}

static std::string readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw arc::ArcError("cannot open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void printUsage(const char* prog) {
    std::cout
        << "usage: " << prog << " [options] file.arc\n"
        << "options:\n"
        << "  -v, --verbose   显示各阶段进度\n"
        << "  --tokens        只跑词法分析并打印 token\n"
        << "  --no-pp         跳过预处理\n"
        << "  --ast           打印程序结构摘要\n"
        << "  -h, --help      显示本帮助\n";
}

static void printAstSummary(const arc::Program& prog) {
    std::cout << "functions: " << prog.functions.size() << "\n";
    for (auto& kv : prog.functions)
        std::cout << "  " << kv.first
                  << "(" << kv.second->params.size() << " params)"
                  << (kv.second->hasBody ? " [body]" : " [decl]")
                  << (kv.second->isEntry ? " [entry]" : "")
                  << (kv.second->isCoroutine ? " [cor]" : "")
                  << "\n";

    if (!prog.compileTimeFunctions.empty()) {
        std::cout << "cte functions: " << prog.compileTimeFunctions.size() << "\n";
        for (auto& kv : prog.compileTimeFunctions)
            std::cout << "  " << kv.first << "\n";
    }
    if (!prog.records.empty()) {
        std::cout << "records: " << prog.records.size() << "\n";
        for (auto& kv : prog.records)
            std::cout << "  " << kv.first << "\n";
    }
    if (!prog.enums.empty()) {
        std::cout << "enums: " << prog.enums.size() << "\n";
        for (auto& kv : prog.enums)
            std::cout << "  " << kv.first << "\n";
    }
    if (!prog.classes.empty()) {
        std::cout << "classes: " << prog.classes.size() << "\n";
        for (auto& kv : prog.classes)
            std::cout << "  " << kv.first
                      << (kv.second->superClass.empty()
                          ? "" : (" sup " + kv.second->superClass))
                      << "\n";
    }

    int gvCount = 0;
    for (auto& tl : prog.topLevel) {
        if (tl.kind != arc::TopLevel::GlobalVar || !tl.stmt) continue;
        auto* vd = dynamic_cast<arc::VarDeclStmt*>(tl.stmt.get());
        if (vd) gvCount += static_cast<int>(vd->vars.size());
    }
    if (gvCount) {
        std::cout << "globals: " << gvCount << "\n";
        for (auto& tl : prog.topLevel) {
            if (tl.kind != arc::TopLevel::GlobalVar || !tl.stmt) continue;
            auto* vd = dynamic_cast<arc::VarDeclStmt*>(tl.stmt.get());
            if (!vd) continue;
            for (auto& it : vd->vars) {
                std::cout << "  " << it.name << ": "
                          << (it.type ? it.type->toString() : "?") << "\n";
            }
        }
    }
}

int main(int argc, char** argv) {
    std::string path;
    bool verbose    = false;
    bool dumpTokens = false;
    bool dumpAst    = false;
    bool noPp       = false;
    std::string emitMode   = "interp";
    std::string targetName = detectHostTarget();
    std::string outPath;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-v" || a == "--verbose") verbose    = true;
        else if (a == "--tokens")               dumpTokens = true;
        else if (a == "--ast")                  dumpAst    = true;
        else if (a == "--no-pp")                noPp       = true;
        else if (a == "-h" || a == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (a.rfind("--emit=", 0) == 0) {
            emitMode = a.substr(7);
        } else if (a.rfind("--target=", 0) == 0) {
            targetName = a.substr(9);
        } else if (a == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "arc: -o requires an output path\n";
                return 1;
            }
            outPath = argv[++i];
        } else if (!a.empty() && a[0] != '-') {
            if (!path.empty()) {
                std::cerr << "arc: multiple input files\n";
                return 1;
            }
            path = a;
        } else {
            std::cerr << "arc: unknown option: " << a << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (path.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    try {
        std::string src = readWholeFile(path);
        if (verbose)
            std::cerr << "[1] read " << path
                      << " (" << src.size() << " bytes)\n";

        std::string processed;
        if (noPp) {
            processed = src;
        } else {
            arc::Preprocessor pp(verbose);
            processed = pp.processSource(src, path);
        }
        if (verbose)
            std::cerr << "[2] preprocessed ("
                      << processed.size() << " bytes)\n";

        arc::Lexer lex(processed);
        auto toks = lex.tokenize();
        if (verbose)
            std::cerr << "[3] " << toks.size() << " tokens\n";

        if (dumpTokens) {
            for (auto& t : toks) {
                std::cout << t.line << ":" << t.col << "\t"
                          << static_cast<int>(t.type)
                          << "\t'" << t.text << "'";
                if (t.type == arc::TT::IntLit) std::cout << "  int=" << t.ival;
                if (t.type == arc::TT::FltLit) std::cout << "  flt=" << t.fval;
                if (t.type == arc::TT::StrLit) std::cout << "  str=\"" << t.sval << "\"";
                if (t.type == arc::TT::ChrLit) std::cout << "  chr='" << t.cval << "'";
                std::cout << "\n";
            }
            return 0;
        }

        arc::Parser parser(std::move(toks));
        auto prog = parser.parseProgram();
        if (verbose)
            std::cerr << "[4] parsed "
                      << prog->topLevel.size() << " top-level items, "
                      << prog->functions.size() << " functions\n";

        if (dumpAst) {
            printAstSummary(*prog);
            return 0;
        }

        std::unique_ptr<arc::Target> target;
        if (targetName == "linux-x86_64")
            target = std::make_unique<arc::LinuxX64Target>();
        else if (targetName == "win-x86_64")
            target = std::make_unique<arc::WinX64Target>();
        else {
            std::cerr << "arc: unknown target: " << targetName << "\n";
            return 1;
        }

        std::unique_ptr<arc::Backend> backend;
        if (emitMode == "nasm") {
            backend = std::make_unique<arc::NasmBackend>(target.get(), outPath);
        } else if (emitMode == "interp") {
            backend = std::make_unique<arc::InterpreterBackend>();
        } else {
            std::cerr << "arc: unknown --emit mode: " << emitMode << "\n";
            return 1;
        }

        std::vector<std::string> userArgs;
        for (int i = 0; i < argc; ++i) {
            std::string a = argv[i];
            if (i > 0) {
                if (a.rfind("--emit=", 0) == 0) continue;
                if (a.rfind("--target=", 0) == 0) continue;
                if (a == "-o") { ++i; continue; }
                if (a.rfind("-o", 0) == 0 && a.length() > 2) continue;
            }
            userArgs.push_back(a);
        }

        int rc = backend->compile(*prog, userArgs);
        if (verbose)
            std::cerr << "[5] exit code " << rc << "\n";
        return rc;

    } catch (arc::ArcError& e) {
        std::cerr << "arc error: " << e.what() << "\n";
        return 1;
    } catch (std::exception& e) {
        std::cerr << "internal error: " << e.what() << "\n";
        return 2;
    }
}
