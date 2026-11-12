#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include "imgui/imgui.h"

class TextEditor
{
public:
    enum class PaletteIndex
    {
        Default,
        Keyword,
        Number,
        String,
        CharLiteral,
        Punctuation,
        Preprocessor,
        Identifier,
        KnownIdentifier,
        PreprocIdentifier,
        Comment,
        MultiLineComment,
        Background,
        Cursor,
        Selection,
        ErrorMarker,
        Breakpoint,
        LineNumber,
        CurrentLineFill,
        CurrentLineFillInactive,
        CurrentLineEdge,
        Max
    };

    struct Coordinates
    {
        int mLine, mColumn;
        Coordinates() : mLine(0), mColumn(0) {}
        Coordinates(int aLine, int aColumn) : mLine(aLine), mColumn(aColumn) {}

        bool operator ==(const Coordinates& o) const
        {
            return mLine == o.mLine && mColumn == o.mColumn;
        }

        bool operator !=(const Coordinates& o) const
        {
            return mLine != o.mLine || mColumn != o.mColumn;
        }

        bool operator <(const Coordinates& o) const
        {
            if (mLine != o.mLine)
                return mLine < o.mLine;
            return mColumn < o.mColumn;
        }

        bool operator >(const Coordinates& o) const
        {
            if (mLine != o.mLine)
                return mLine > o.mLine;
            return mColumn > o.mColumn;
        }

        bool operator <=(const Coordinates& o) const
        {
            if (mLine != o.mLine)
                return mLine < o.mLine;
            return mColumn <= o.mColumn;
        }

        bool operator >=(const Coordinates& o) const
        {
            if (mLine != o.mLine)
                return mLine > o.mLine;
            return mColumn >= o.mColumn;
        }
    };

    struct Glyph
    {
        char mChar;
        PaletteIndex mColorIndex;
        Glyph(char aChar, PaletteIndex aColorIndex) : mChar(aChar), mColorIndex(aColorIndex) {}
    };

    typedef std::vector<Glyph> Line;
    typedef std::vector<Line> Lines;

    TextEditor();
    ~TextEditor();

    void SetText(const std::string& aText);
    std::string GetText() const;

    void Render(const char* aTitle, const ImVec2& aSize = ImVec2(), bool aBorder = false);
    
    void SetPalette(const std::vector<ImU32>& aValue) { mPalette = aValue; }
    static std::vector<ImU32> GetDarkPalette();

    void InsertText(const std::string& aValue);
    void MoveUp(int aAmount = 1, bool aSelect = false);
    void MoveDown(int aAmount = 1, bool aSelect = false);
    void MoveLeft(int aAmount = 1, bool aSelect = false);
    void MoveRight(int aAmount = 1, bool aSelect = false);
    void MoveTop(bool aSelect = false);
    void MoveBottom(bool aSelect = false);
    void MoveHome(bool aSelect = false);
    void MoveEnd(bool aSelect = false);

    void SetCursorPosition(const Coordinates& aPosition);
    Coordinates GetCursorPosition() const { return mState.mCursorPosition; }

    void EnterCharacter(ImWchar aChar, bool aShift);
    void Backspace();
    void Delete();
    void DeleteWordLeft();
    void SelectAll();
    
    bool HasSelection() const;

private:
    struct EditorState
    {
        Coordinates mCursorPosition;
        Coordinates mSelectionStart;
        Coordinates mSelectionEnd;

        EditorState() : mCursorPosition(), mSelectionStart(), mSelectionEnd() {}
    };

    void Colorize(int aFromLine = 0, int aLines = -1);
    void ColorizeRange(int aFromLine = 0, int aToLine = 0);
    void ColorizeInternal();
    
    float TextDistanceToLineStart(const Coordinates& aFrom) const;
    void EnsureCursorVisible();
    int GetPageSize() const;
    std::string GetText(const Coordinates& aStart, const Coordinates& aEnd) const;
    Coordinates GetActualCursorCoordinates() const;
    Coordinates SanitizeCoordinates(const Coordinates& aValue) const;
    void Advance(Coordinates& aCoordinates) const;
    void DeleteRange(const Coordinates& aStart, const Coordinates& aEnd);
    int InsertTextAt(Coordinates& aWhere, const char* aValue);
    void AddUndo(EditorState& aValue);
    Coordinates ScreenPosToCoordinates(const ImVec2& aPosition) const;
    Coordinates FindWordStart(const Coordinates& aFrom) const;
    Coordinates FindWordEnd(const Coordinates& aFrom) const;
    bool IsOnWordBoundary(const Coordinates& aAt) const;
    void RemoveLine(int aStart, int aEnd);
    void RemoveLine(int aIndex);
    Line& InsertLine(int aIndex);
    void EnterCharacterInternal(ImWchar aChar, bool aShift);
    void DeleteSelection();
    std::string GetWordUnderCursor() const;
    std::string GetWordAt(const Coordinates& aCoords) const;
    ImU32 GetGlyphColor(const Glyph& aGlyph) const;

    void HandleKeyboardInputs();
    void HandleMouseInputs();
    void Render();
    void SmoothScrollTo(float y);

    float mLineSpacing;
    Lines mLines;
    EditorState mState;
    std::vector<ImU32> mPalette;
    bool mWithinRender;
    bool mScrollToCursor;
    bool mScrollToTop;
    bool mTextChanged;
    int mColorRangeMin, mColorRangeMax;
    int mUndoIndex;
    int mTabSize;
    bool mOverwrite;
    bool mReadOnly;
    bool mCursorPositionChanged;
    int mLeftMargin;
    float mTextStart;
    float mLastClick;
    float mDesiredScrollY;
    float mCursorBlinkTime;
    bool mIsFocused;

    std::set<std::string> mLuaKeywords;
};

