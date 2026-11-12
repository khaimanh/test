#include "TextEditor.hpp"
#include <algorithm>
#include <chrono>
#include <string>
#include <cmath>

static int UTF8CharLength(char c)
{
    if ((c & 0xFE) == 0xFC)
        return 6;
    if ((c & 0xFC) == 0xF8)
        return 5;
    if ((c & 0xF8) == 0xF0)
        return 4;
    else if ((c & 0xF0) == 0xE0)
        return 3;
    else if ((c & 0xE0) == 0xC0)
        return 2;
    return 1;
}

TextEditor::TextEditor()
    : mLineSpacing(1.0f)
    , mWithinRender(false)
    , mScrollToCursor(false)
    , mScrollToTop(false)
    , mTextChanged(false)
    , mColorRangeMin(0)
    , mColorRangeMax(0)
    , mUndoIndex(0)
    , mTabSize(4)
    , mOverwrite(false)
    , mReadOnly(false)
    , mCursorPositionChanged(false)
    , mLeftMargin(10)
    , mTextStart(60.0f)
    , mLastClick(-1.0f)
    , mDesiredScrollY(0.0f)
    , mCursorBlinkTime(0.0f)
    , mIsFocused(false)
{
    SetPalette(GetDarkPalette());
    mLines.push_back(Line());

    mLuaKeywords = {
        "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
        "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true",
        "until", "while", "goto", "print"
    };
}

TextEditor::~TextEditor()
{
}

void TextEditor::SetText(const std::string& aText)
{
    mLines.clear();
    mLines.push_back(Line());

    for (auto chr : aText)
    {
        if (chr == '\r')
        {
        }
        else if (chr == '\n')
            mLines.push_back(Line());
        else
        {
            mLines.back().push_back(Glyph(chr, PaletteIndex::Default));
        }
    }

    mTextChanged = true;
    mScrollToTop = true;

    Colorize();
}

std::string TextEditor::GetText() const
{
    return GetText(Coordinates(), Coordinates((int)mLines.size(), 0));
}

std::string TextEditor::GetText(const Coordinates& aStart, const Coordinates& aEnd) const
{
    std::string result;

    auto start = aStart;
    auto end = aEnd;
    if (start > end)
        std::swap(start, end);

    for (auto lindex = start.mLine; lindex < (int)mLines.size(); ++lindex)
    {
        auto& line = mLines[lindex];
        if (lindex == start.mLine)
        {
            for (int i = start.mColumn; i < (int)line.size() && (lindex != end.mLine || i < end.mColumn); ++i)
                result += line[i].mChar;
        }
        else if (lindex == end.mLine)
        {
            for (int i = 0; i < end.mColumn && i < (int)line.size(); ++i)
                result += line[i].mChar;
        }
        else
        {
            for (auto& g : line)
                result += g.mChar;
        }

        if (lindex < end.mLine)
            result += '\n';
    }

    return result;
}

void TextEditor::SetCursorPosition(const Coordinates& aPosition)
{
    if (mState.mCursorPosition != aPosition)
    {
        mState.mCursorPosition = aPosition;
        mCursorPositionChanged = true;
        mCursorBlinkTime = 0.0f;
        EnsureCursorVisible();
    }
}

void TextEditor::EnterCharacter(ImWchar aChar, bool aShift)
{
    if (mReadOnly)
        return;
    
    mCursorBlinkTime = 0.0f;

    if (HasSelection())
        DeleteSelection();

    auto coord = GetActualCursorCoordinates();
    auto& line = mLines[coord.mLine];

    if (aChar == '\n')
    {
        InsertLine(coord.mLine + 1);
        auto& newLine = mLines[coord.mLine + 1];
        
        if (coord.mColumn < (int)line.size())
        {
            newLine.insert(newLine.begin(), line.begin() + coord.mColumn, line.end());
            line.erase(line.begin() + coord.mColumn, line.end());
        }

        SetCursorPosition(Coordinates(coord.mLine + 1, 0));
    }
    else
    {
        char c = (char)aChar;
        if (coord.mColumn < (int)line.size())
            line.insert(line.begin() + coord.mColumn, Glyph(c, PaletteIndex::Default));
        else
            line.push_back(Glyph(c, PaletteIndex::Default));
        
        SetCursorPosition(Coordinates(coord.mLine, coord.mColumn + 1));
    }

    mTextChanged = true;
    Colorize(coord.mLine, 1);
}

void TextEditor::Backspace()
{
    if (mReadOnly)
        return;

    if (HasSelection())
    {
        DeleteSelection();
        return;
    }

    auto coord = GetActualCursorCoordinates();

    if (coord.mColumn == 0)
    {
        if (coord.mLine > 0)
        {
            auto& prevLine = mLines[coord.mLine - 1];
            auto prevSize = (int)prevLine.size();
            auto& currentLine = mLines[coord.mLine];

            prevLine.insert(prevLine.end(), currentLine.begin(), currentLine.end());
            RemoveLine(coord.mLine);
            SetCursorPosition(Coordinates(coord.mLine - 1, prevSize));
        }
    }
    else
    {
        auto& line = mLines[coord.mLine];
        line.erase(line.begin() + coord.mColumn - 1);
        SetCursorPosition(Coordinates(coord.mLine, coord.mColumn - 1));
    }

    mTextChanged = true;
    Colorize(coord.mLine, 1);
}

void TextEditor::Delete()
{
    if (mReadOnly)
        return;

    if (HasSelection())
    {
        DeleteSelection();
        return;
    }

    auto coord = GetActualCursorCoordinates();
    auto& line = mLines[coord.mLine];

    if (coord.mColumn >= (int)line.size())
    {
        if (coord.mLine < (int)mLines.size() - 1)
        {
            auto& nextLine = mLines[coord.mLine + 1];
            line.insert(line.end(), nextLine.begin(), nextLine.end());
            RemoveLine(coord.mLine + 1);
        }
    }
    else
    {
        line.erase(line.begin() + coord.mColumn);
    }

    mTextChanged = true;
    Colorize(coord.mLine, 1);
}

void TextEditor::DeleteWordLeft()
{
    if (mReadOnly)
        return;

    auto coord = GetActualCursorCoordinates();
    auto& line = mLines[coord.mLine];

    if (coord.mColumn == 0)
    {
        if (coord.mLine > 0)
        {
            auto& prevLine = mLines[coord.mLine - 1];
            auto prevSize = (int)prevLine.size();
            prevLine.insert(prevLine.end(), line.begin(), line.end());
            RemoveLine(coord.mLine);
            SetCursorPosition(Coordinates(coord.mLine - 1, prevSize));
        }
    }
    else
    {
        auto start = FindWordStart(coord);
        if (start.mLine == coord.mLine)
        {
            line.erase(line.begin() + start.mColumn, line.begin() + coord.mColumn);
            SetCursorPosition(start);
        }
    }

    mTextChanged = true;
    Colorize(coord.mLine, 1);
}

void TextEditor::SelectAll()
{
    mState.mSelectionStart = Coordinates(0, 0);
    mState.mSelectionEnd = Coordinates((int)mLines.size() - 1, (int)mLines.back().size());
    mState.mCursorPosition = mState.mSelectionEnd;
}

bool TextEditor::HasSelection() const
{
    return mState.mSelectionStart != mState.mSelectionEnd;
}

void TextEditor::DeleteSelection()
{
    if (mReadOnly)
        return;

    if (!HasSelection())
        return;

    DeleteRange(mState.mSelectionStart, mState.mSelectionEnd);
    SetCursorPosition(mState.mSelectionStart);
    mState.mSelectionStart = mState.mSelectionEnd = mState.mCursorPosition;
    mTextChanged = true;
    Colorize();
}

void TextEditor::MoveUp(int aAmount, bool aSelect)
{
    auto oldPos = mState.mCursorPosition;
    mState.mCursorPosition.mLine = std::max(0, mState.mCursorPosition.mLine - aAmount);
    if (oldPos != mState.mCursorPosition)
    {
        if (aSelect)
        {
            if (oldPos == mState.mSelectionStart)
                mState.mSelectionStart = mState.mCursorPosition;
            else if (oldPos == mState.mSelectionEnd)
                mState.mSelectionEnd = mState.mCursorPosition;
            else
            {
                mState.mSelectionStart = mState.mCursorPosition;
                mState.mSelectionEnd = oldPos;
            }
        }
        else
            mState.mSelectionStart = mState.mSelectionEnd = mState.mCursorPosition;
        
        EnsureCursorVisible();
        mCursorPositionChanged = true;
    }
}

void TextEditor::MoveDown(int aAmount, bool aSelect)
{
    auto oldPos = mState.mCursorPosition;
    mState.mCursorPosition.mLine = std::max(0, std::min((int)mLines.size() - 1, mState.mCursorPosition.mLine + aAmount));
    if (mState.mCursorPosition.mLine != oldPos.mLine)
    {
        if (aSelect)
        {
            if (oldPos == mState.mSelectionStart)
                mState.mSelectionStart = mState.mCursorPosition;
            else if (oldPos == mState.mSelectionEnd)
                mState.mSelectionEnd = mState.mCursorPosition;
            else
            {
                mState.mSelectionStart = oldPos;
                mState.mSelectionEnd = mState.mCursorPosition;
            }
        }
        else
            mState.mSelectionStart = mState.mSelectionEnd = mState.mCursorPosition;
        
        EnsureCursorVisible();
        mCursorPositionChanged = true;
    }
}

void TextEditor::MoveLeft(int aAmount, bool aSelect)
{
    if (mLines.empty())
        return;

    auto oldPos = mState.mCursorPosition;
    mState.mCursorPosition = GetActualCursorCoordinates();

    while (aAmount-- > 0)
    {
        if (mState.mCursorPosition.mColumn == 0)
        {
            if (mState.mCursorPosition.mLine > 0)
            {
                --mState.mCursorPosition.mLine;
                mState.mCursorPosition.mColumn = (int)mLines[mState.mCursorPosition.mLine].size();
            }
        }
        else
        {
            mState.mCursorPosition.mColumn = std::max(0, mState.mCursorPosition.mColumn - 1);
        }
    }

    if (oldPos != mState.mCursorPosition)
    {
        if (aSelect)
        {
            if (oldPos == mState.mSelectionStart)
                mState.mSelectionStart = mState.mCursorPosition;
            else if (oldPos == mState.mSelectionEnd)
                mState.mSelectionEnd = mState.mCursorPosition;
            else
            {
                mState.mSelectionStart = mState.mCursorPosition;
                mState.mSelectionEnd = oldPos;
            }
        }
        else
            mState.mSelectionStart = mState.mSelectionEnd = mState.mCursorPosition;
        
        EnsureCursorVisible();
        mCursorPositionChanged = true;
    }
}

void TextEditor::MoveRight(int aAmount, bool aSelect)
{
    auto oldPos = mState.mCursorPosition;

    if (mLines.empty() || oldPos.mLine >= mLines.size())
        return;

    auto cindex = GetActualCursorCoordinates();
    while (aAmount-- > 0)
    {
        auto lindex = cindex.mLine;
        auto& line = mLines[lindex];

        if (cindex.mColumn >= line.size())
        {
            if (cindex.mLine < mLines.size() - 1)
            {
                cindex.mLine = std::max(0, std::min((int)mLines.size() - 1, cindex.mLine + 1));
                cindex.mColumn = 0;
            }
        }
        else
        {
            cindex.mColumn = cindex.mColumn + 1;
        }
    }

    mState.mCursorPosition = cindex;

    if (oldPos != mState.mCursorPosition)
    {
        if (aSelect)
        {
            if (oldPos == mState.mSelectionStart)
                mState.mSelectionStart = mState.mCursorPosition;
            else if (oldPos == mState.mSelectionEnd)
                mState.mSelectionEnd = mState.mCursorPosition;
            else
            {
                mState.mSelectionStart = oldPos;
                mState.mSelectionEnd = mState.mCursorPosition;
            }
        }
        else
            mState.mSelectionStart = mState.mSelectionEnd = mState.mCursorPosition;
        
        EnsureCursorVisible();
        mCursorPositionChanged = true;
    }
}

void TextEditor::MoveTop(bool aSelect)
{
    auto oldPos = mState.mCursorPosition;
    SetCursorPosition(Coordinates(0, 0));

    if (mState.mCursorPosition != oldPos)
    {
        mScrollToTop = true;
        mCursorPositionChanged = true;
    }
}

void TextEditor::MoveBottom(bool aSelect)
{
    auto oldPos = mState.mCursorPosition;
    auto line = std::max(0, (int)mLines.size() - 1);
    SetCursorPosition(Coordinates(line, 0));

    if (oldPos != mState.mCursorPosition)
    {
        mCursorPositionChanged = true;
    }
}

void TextEditor::MoveHome(bool aSelect)
{
    auto oldPos = mState.mCursorPosition;
    SetCursorPosition(Coordinates(mState.mCursorPosition.mLine, 0));

    if (mState.mCursorPosition != oldPos)
        mCursorPositionChanged = true;
}

void TextEditor::MoveEnd(bool aSelect)
{
    auto oldPos = mState.mCursorPosition;
    SetCursorPosition(Coordinates(mState.mCursorPosition.mLine, (int)mLines[oldPos.mLine].size()));

    if (mState.mCursorPosition != oldPos)
        mCursorPositionChanged = true;
}

void TextEditor::EnsureCursorVisible()
{
    mScrollToCursor = true;
}

TextEditor::Coordinates TextEditor::GetActualCursorCoordinates() const
{
    return SanitizeCoordinates(mState.mCursorPosition);
}

TextEditor::Coordinates TextEditor::SanitizeCoordinates(const Coordinates& aValue) const
{
    auto line = aValue.mLine;
    auto column = aValue.mColumn;
    if (line >= (int)mLines.size())
    {
        if (mLines.empty())
        {
            line = 0;
            column = 0;
        }
        else
        {
            line = (int)mLines.size() - 1;
            column = (int)mLines[line].size();
        }
        return Coordinates(line, column);
    }
    else
    {
        column = mLines.empty() ? 0 : std::min(column, (int)mLines[line].size());
        return Coordinates(line, column);
    }
}

void TextEditor::Advance(Coordinates& aCoordinates) const
{
    if (aCoordinates.mLine < (int)mLines.size())
    {
        auto& line = mLines[aCoordinates.mLine];
        auto cindex = aCoordinates.mColumn;

        if (cindex + 1 < (int)line.size())
            ++aCoordinates.mColumn;
        else
        {
            ++aCoordinates.mLine;
            aCoordinates.mColumn = 0;
        }
    }
}

void TextEditor::RemoveLine(int aIndex)
{
    if (aIndex >= 0 && aIndex < (int)mLines.size())
    {
        mLines.erase(mLines.begin() + aIndex);
    }
}

void TextEditor::RemoveLine(int aStart, int aEnd)
{
    if (aStart >= 0 && aStart < (int)mLines.size() && aEnd >= aStart && aEnd <= (int)mLines.size())
    {
        mLines.erase(mLines.begin() + aStart, mLines.begin() + aEnd);
    }
}

TextEditor::Line& TextEditor::InsertLine(int aIndex)
{
    auto& result = *mLines.insert(mLines.begin() + aIndex, Line());
    return result;
}

void TextEditor::Colorize(int aFromLine, int aCount)
{
    int toLine = aCount == -1 ? (int)mLines.size() : std::min((int)mLines.size(), aFromLine + aCount);
    mColorRangeMin = std::min(mColorRangeMin, aFromLine);
    mColorRangeMax = std::max(mColorRangeMax, toLine);
    mColorRangeMin = std::max(0, mColorRangeMin);
    mColorRangeMax = std::max(mColorRangeMin, mColorRangeMax);
    ColorizeRange(aFromLine, toLine);
}

void TextEditor::ColorizeRange(int aFromLine, int aToLine)
{
    if (mLines.empty() || aFromLine >= aToLine)
        return;

    std::string buffer;
    for (int i = aFromLine; i < aToLine; ++i)
    {
        auto& line = mLines[i];

        if (line.empty())
            continue;

        buffer.resize(line.size());
        for (size_t j = 0; j < line.size(); ++j)
            buffer[j] = line[j].mChar;

        const char* bufferBegin = &buffer.front();
        const char* bufferEnd = bufferBegin + buffer.size();

        auto last = PaletteIndex::Default;

        for (const char* token_begin = bufferBegin; token_begin < bufferEnd; )
        {
            const char* token_end = token_begin;

            auto id = PaletteIndex::Default;

            bool hasTokenizeResult = false;

            if (*token_begin == '"')
            {
                token_end++;
                while (token_end < bufferEnd && *token_end != '"')
                {
                    if (*token_end == '\\')
                    {
                        token_end++;
                        if (token_end < bufferEnd)
                            token_end++;
                    }
                    else
                        token_end++;
                }
                if (token_end < bufferEnd)
                    token_end++;
                id = PaletteIndex::String;
                hasTokenizeResult = true;
            }
            else if (*token_begin == '\'')
            {
                token_end++;
                while (token_end < bufferEnd && *token_end != '\'')
                {
                    if (*token_end == '\\')
                    {
                        token_end++;
                        if (token_end < bufferEnd)
                            token_end++;
                    }
                    else
                        token_end++;
                }
                if (token_end < bufferEnd)
                    token_end++;
                id = PaletteIndex::CharLiteral;
                hasTokenizeResult = true;
            }
            else if (token_begin < bufferEnd - 1 && token_begin[0] == '-' && token_begin[1] == '-')
            {
                token_end = bufferEnd;
                id = PaletteIndex::Comment;
                hasTokenizeResult = true;
            }
            else if (isdigit(*token_begin))
            {
                token_end = token_begin + 1;
                while (token_end < bufferEnd && (isdigit(*token_end) || *token_end == '.'))
                    token_end++;
                id = PaletteIndex::Number;
                hasTokenizeResult = true;
            }
            else if (isalpha(*token_begin) || *token_begin == '_')
            {
                token_end = token_begin + 1;
                while (token_end < bufferEnd && (isalnum(*token_end) || *token_end == '_'))
                    token_end++;

                std::string word(token_begin, token_end);
                if (mLuaKeywords.find(word) != mLuaKeywords.end())
                    id = PaletteIndex::Keyword;
                else
                    id = PaletteIndex::Identifier;

                hasTokenizeResult = true;
            }

            if (!hasTokenizeResult)
            {
                if (ispunct(*token_begin))
                    id = PaletteIndex::Punctuation;
                else if (isblank(*token_begin))
                    id = PaletteIndex::Default;
                token_end = token_begin + 1;
            }

            for (auto token_it = token_begin; token_it < token_end; ++token_it)
            {
                size_t index = token_it - bufferBegin;
                if (index < line.size())
                    line[index].mColorIndex = id;
            }

            token_begin = token_end;
        }
    }
}

void TextEditor::ColorizeInternal()
{
    if (mLines.empty())
        return;

    if (mColorRangeMin < mColorRangeMax)
    {
        const int increment = (mColorRangeMax - mColorRangeMin) / 10 + 1;
        const int to = std::min(mColorRangeMin + increment, mColorRangeMax);
        ColorizeRange(mColorRangeMin, to);
        mColorRangeMin = to;

        if (mColorRangeMax == mColorRangeMin)
        {
            mColorRangeMin = INT_MAX;
            mColorRangeMax = 0;
        }
    }
}

std::vector<ImU32> TextEditor::GetDarkPalette()
{
    std::vector<ImU32> p(static_cast<int>(PaletteIndex::Max));

    p[(int)PaletteIndex::Default] = IM_COL32(208, 208, 208, 255);
    p[(int)PaletteIndex::Keyword] = IM_COL32(78, 201, 139, 255);
    p[(int)PaletteIndex::Number] = IM_COL32(181, 206, 168, 255);
    p[(int)PaletteIndex::String] = IM_COL32(214, 157, 133, 255);
    p[(int)PaletteIndex::CharLiteral] = IM_COL32(214, 157, 133, 255);
    p[(int)PaletteIndex::Punctuation] = IM_COL32(208, 208, 208, 255);
    p[(int)PaletteIndex::Preprocessor] = IM_COL32(197, 134, 192, 255);
    p[(int)PaletteIndex::Identifier] = IM_COL32(156, 220, 254, 255);
    p[(int)PaletteIndex::KnownIdentifier] = IM_COL32(78, 201, 176, 255);
    p[(int)PaletteIndex::PreprocIdentifier] = IM_COL32(197, 134, 192, 255);
    p[(int)PaletteIndex::Comment] = IM_COL32(106, 153, 85, 255);
    p[(int)PaletteIndex::MultiLineComment] = IM_COL32(106, 153, 85, 255);
    p[(int)PaletteIndex::Background] = IM_COL32(22, 22, 24, 255);
    p[(int)PaletteIndex::Cursor] = IM_COL32(255, 255, 255, 255);
    p[(int)PaletteIndex::Selection] = IM_COL32(38, 79, 120, 128);
    p[(int)PaletteIndex::ErrorMarker] = IM_COL32(255, 0, 0, 255);
    p[(int)PaletteIndex::Breakpoint] = IM_COL32(128, 240, 0, 64);
    p[(int)PaletteIndex::LineNumber] = IM_COL32(133, 133, 133, 255);
    p[(int)PaletteIndex::CurrentLineFill] = IM_COL32(0, 0, 0, 64);
    p[(int)PaletteIndex::CurrentLineFillInactive] = IM_COL32(0, 0, 0, 64);
    p[(int)PaletteIndex::CurrentLineEdge] = IM_COL32(160, 160, 160, 64);

    return p;
}

ImU32 TextEditor::GetGlyphColor(const Glyph& aGlyph) const
{
    return mPalette[(int)aGlyph.mColorIndex];
}

void TextEditor::HandleKeyboardInputs()
{
    ImGuiIO& io = ImGui::GetIO();
    auto shift = io.KeyShift;
    auto ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
    auto alt = io.ConfigMacOSXBehaviors ? io.KeyCtrl : io.KeyAlt;

    if (ImGui::IsWindowFocused() && mIsFocused)
    {
        if (ImGui::IsWindowHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

        io.WantCaptureKeyboard = true;
        io.WantTextInput = true;

        if (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_A)))
            SelectAll();
        else if (!mReadOnly && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Z)))
        {
        }
        else if (!mReadOnly && ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Backspace)))
            DeleteWordLeft();
        else if (!mReadOnly && !ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Delete)))
            Delete();
        else if (!mReadOnly && !ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Backspace)))
            Backspace();
        else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_UpArrow)))
            MoveUp(1, shift);
        else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_DownArrow)))
            MoveDown(1, shift);
        else if (!alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_LeftArrow)))
            MoveLeft(1, shift);
        else if (!alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_RightArrow)))
            MoveRight(1, shift);
        else if (!alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_PageUp)))
            MoveUp(GetPageSize() - 4, shift);
        else if (!alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_PageDown)))
            MoveDown(GetPageSize() - 4, shift);
        else if (!alt && ctrl && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Home)))
            MoveTop(shift);
        else if (ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_End)))
            MoveBottom(shift);
        else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Home)))
            MoveHome(shift);
        else if (!ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_End)))
            MoveEnd(shift);
        else if (!mReadOnly && !ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Enter)))
            EnterCharacter('\n', shift);
        else if (!mReadOnly && !ctrl && !alt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Tab)))
            EnterCharacter('\t', shift);

        if (!mReadOnly && !io.InputQueueCharacters.empty())
        {
            for (int i = 0; i < io.InputQueueCharacters.Size; i++)
            {
                auto c = io.InputQueueCharacters[i];
                if (c != 0 && (c == '\n' || c >= 32))
                    EnterCharacter(c, shift);
            }
            io.InputQueueCharacters.resize(0);
        }
    }
}

void TextEditor::HandleMouseInputs()
{
    ImGuiIO& io = ImGui::GetIO();
    auto shift = io.KeyShift;
    auto ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
    auto alt = io.ConfigMacOSXBehaviors ? io.KeyCtrl : io.KeyAlt;

    if (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered())
    {
        mIsFocused = false;
    }

    if (ImGui::IsWindowHovered() || (ImGui::IsWindowFocused() && ImGui::IsMouseDown(0)))
    {
        auto click = ImGui::IsMouseClicked(0);
        auto doubleClick = ImGui::IsMouseDoubleClicked(0);
        auto t = ImGui::GetTime();
        auto tripleClick = click && !doubleClick && (mLastClick != -1.0f && (t - mLastClick) < 0.3f);

        if (click)
        {
            mIsFocused = true;
        }

        if (tripleClick)
        {
            auto pos = ImGui::GetMousePos();
            auto coord = ScreenPosToCoordinates(pos);
            mState.mCursorPosition = coord;
            mState.mSelectionStart = Coordinates(coord.mLine, 0);
            mState.mSelectionEnd = Coordinates(coord.mLine, (int)mLines[coord.mLine].size());
            mLastClick = -1.0f;
        }
        else
        {
            if (doubleClick)
            {
                auto pos = ImGui::GetMousePos();
                auto coord = ScreenPosToCoordinates(pos);
                mState.mCursorPosition = coord;
                mState.mSelectionStart = FindWordStart(coord);
                mState.mSelectionEnd = FindWordEnd(coord);
            }
            else if (click)
            {
                auto pos = ImGui::GetMousePos();
                auto coord = ScreenPosToCoordinates(pos);
                
                if (shift)
                {
                    if (mState.mSelectionStart == mState.mSelectionEnd)
                        mState.mSelectionStart = mState.mCursorPosition;
                    mState.mCursorPosition = coord;
                    mState.mSelectionEnd = coord;
                }
                else
                {
                    mState.mCursorPosition = coord;
                    mState.mSelectionStart = mState.mSelectionEnd = coord;
                }
                mLastClick = (float)ImGui::GetTime();
            }
            else if (ImGui::IsMouseDragging(0))
            {
                auto pos = ImGui::GetMousePos();
                auto winPos = ImGui::GetWindowPos();
                auto winSize = ImGui::GetWindowSize();
                float edge = 8.0f;

                if (pos.y < winPos.y + edge)
                    SmoothScrollTo(ImGui::GetScrollY() - ImGui::GetTextLineHeightWithSpacing());
                else if (pos.y > winPos.y + winSize.y - edge)
                    SmoothScrollTo(ImGui::GetScrollY() + ImGui::GetTextLineHeightWithSpacing());

                auto coord = ScreenPosToCoordinates(pos);

                if (mState.mSelectionStart == mState.mSelectionEnd)
                    mState.mSelectionStart = mState.mCursorPosition;

                mState.mCursorPosition = coord;
                mState.mSelectionEnd = coord;
            }
        }
    }
}

int TextEditor::GetPageSize() const
{
    auto height = ImGui::GetWindowHeight();
    return (int)floor(height / ImGui::GetTextLineHeightWithSpacing());
}

TextEditor::Coordinates TextEditor::ScreenPosToCoordinates(const ImVec2& aPosition) const
{
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 local(aPosition.x - origin.x, aPosition.y - origin.y);

    int lineNo = std::max(0, (int)floor(local.y / ImGui::GetTextLineHeightWithSpacing()));
    int columnCoord = 0;

    if (lineNo >= 0 && lineNo < (int)mLines.size())
    {
        auto& line = mLines.at(lineNo);

        auto distance = 0.0f;
        auto spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ").x;

        for (size_t it = 0; it < line.size(); ++it)
        {
            float width = 0.0f;

            if (line[it].mChar == '\t')
            {
                width = spaceSize * mTabSize;
            }
            else
            {
                char buf[2];
                buf[0] = line[it].mChar;
                buf[1] = 0;
                width = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf).x;
            }

            if (mTextStart + distance + width * 0.5f > local.x)
                break;

            distance += width;
            ++columnCoord;
        }
    }

    return SanitizeCoordinates(Coordinates(lineNo, columnCoord));
}

void TextEditor::Render(const char* aTitle, const ImVec2& aSize, bool aBorder)
{
    mWithinRender = true;
    mTextChanged = false;
    mCursorPositionChanged = false;
    
    mCursorBlinkTime += ImGui::GetIO().DeltaTime;
    bool showCursor = fmod(mCursorBlinkTime, 1.0f) < 0.5f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(mPalette[(int)PaletteIndex::Background]));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.1f, 0.1f, 0.1f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(129 / 255.0f, 35 / 255.0f, 34 / 255.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(154 / 255.0f, 42 / 255.0f, 43 / 255.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(165 / 255.0f, 48 / 255.0f, 49 / 255.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 5.0f);
    if (!ImGui::BeginChild(aTitle, aSize, aBorder, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
        return;
    }

    HandleKeyboardInputs();
    HandleMouseInputs();
    ColorizeInternal();

    ImGui::PushAllowKeyboardFocus(true);

    auto contentSize = ImGui::GetWindowContentRegionMax();
    auto drawList = ImGui::GetWindowDrawList();
    float longest = mTextStart;

    auto lineMax = (int)mLines.size();
    auto globalLineMax = lineMax;

    ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
    auto scrollX = ImGui::GetScrollX();
    auto scrollY = ImGui::GetScrollY();
    
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
    {
        float scrollAmount = ImGui::GetIO().MouseWheel * ImGui::GetTextLineHeightWithSpacing() * 3.0f;
        mDesiredScrollY = std::max(0.0f, mDesiredScrollY - scrollAmount);
    }
    
    if (abs(mDesiredScrollY - scrollY) > 0.5f)
    {
        float dt = (float)ImGui::GetIO().DeltaTime;
        float speed = 15.0f;
        float newY = scrollY + (mDesiredScrollY - scrollY) * std::min(1.0f, speed * dt);
        ImGui::SetScrollY(newY);
        scrollY = newY;
    }
    else
    {
        mDesiredScrollY = scrollY;
    }

    auto lineNo = (int)floor(scrollY / ImGui::GetTextLineHeightWithSpacing());
    auto lineMax2 = std::max(0, std::min((int)mLines.size() - 1, lineNo + (int)floor((scrollY + contentSize.y) / ImGui::GetTextLineHeightWithSpacing())));

    if (!mLines.empty())
    {
        auto fontScale = ImGui::GetFontSize() / ImGui::GetFont()->FontSize;
        float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr).x;

        int digits = std::max(1, (int)std::floor(std::log10((double)std::max(1, globalLineMax))) + 1);
        char widest[16];
        for (int i = 0; i < std::min(15, digits); ++i) widest[i] = '8';
        widest[std::min(15, digits)] = '\0';
        float numberWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, widest, nullptr, nullptr).x;
        float gutterPadding = 8.0f;
        mTextStart = 6.0f + numberWidth + gutterPadding;

        while (lineNo <= lineMax2)
        {
            ImVec2 lineStartScreenPos = ImVec2(cursorScreenPos.x, cursorScreenPos.y + lineNo * ImGui::GetTextLineHeightWithSpacing());
            ImVec2 textScreenPos = ImVec2(lineStartScreenPos.x + mTextStart, lineStartScreenPos.y);

            auto& line = mLines[lineNo];
            longest = std::max(mTextStart + TextDistanceToLineStart(Coordinates(lineNo, (int)line.size())), longest);
            auto columnNo = 0;
            Coordinates lineStartCoord(lineNo, 0);
            Coordinates lineEndCoord(lineNo, (int)line.size());

            float sstart = -1.0f;
            float ssend = -1.0f;

            if (mState.mSelectionStart <= lineEndCoord && mState.mSelectionEnd >= lineStartCoord && mState.mSelectionStart != mState.mSelectionEnd)
            {
                auto selectionStart = mState.mSelectionStart < mState.mSelectionEnd ? mState.mSelectionStart : mState.mSelectionEnd;
                auto selectionEnd = mState.mSelectionStart < mState.mSelectionEnd ? mState.mSelectionEnd : mState.mSelectionStart;

                if (lineNo == selectionStart.mLine && lineNo == selectionEnd.mLine)
                {
                    sstart = TextDistanceToLineStart(selectionStart);
                    ssend = TextDistanceToLineStart(selectionEnd);
                }
                else if (lineNo == selectionStart.mLine)
                {
                    sstart = TextDistanceToLineStart(selectionStart);
                    ssend = TextDistanceToLineStart(lineEndCoord);
                }
                else if (lineNo == selectionEnd.mLine)
                {
                    sstart = 0.0f;
                    ssend = TextDistanceToLineStart(selectionEnd);
                }
                else
                {
                    sstart = 0.0f;
                    ssend = TextDistanceToLineStart(lineEndCoord);
                }

                if (sstart >= 0.0f && ssend >= 0.0f)
                {
                    ImVec2 vstart(lineStartScreenPos.x + mTextStart + sstart, lineStartScreenPos.y);
                    ImVec2 vend(lineStartScreenPos.x + mTextStart + ssend, lineStartScreenPos.y + ImGui::GetTextLineHeightWithSpacing());
                    drawList->AddRectFilled(vstart, vend, mPalette[(int)PaletteIndex::Selection]);
                }
            }

            if (mState.mCursorPosition.mLine == lineNo && showCursor && mIsFocused)
            {
                ImVec2 cmin = ImVec2(textScreenPos.x + TextDistanceToLineStart(mState.mCursorPosition), lineStartScreenPos.y);
                ImVec2 cmax = ImVec2(cmin.x + 1.0f, lineStartScreenPos.y + ImGui::GetTextLineHeightWithSpacing());
                drawList->AddRectFilled(cmin, cmax, mPalette[(int)PaletteIndex::Cursor]);
            }

            char buf[16];
            snprintf(buf, 16, "%d", lineNo + 1);
            float lineLeft = cursorScreenPos.x + 6.0f;
            drawList->AddText(ImVec2(lineLeft, lineStartScreenPos.y), mPalette[(int)PaletteIndex::LineNumber], buf);

            if (mCursorPositionChanged)
            {
                if (mState.mCursorPosition.mLine == lineNo)
                {
                    float cx = TextDistanceToLineStart(mState.mCursorPosition) + mTextStart;
                    if (cx < scrollX)
                        ImGui::SetScrollX(cx);
                    if (cx > scrollX + contentSize.x)
                        ImGui::SetScrollX(cx - contentSize.x);
                }
            }

            for (size_t i = 0; i < line.size();)
            {
                auto& glyph = line[i];
                auto color = GetGlyphColor(glyph);

                if (glyph.mChar == '\t')
                {
                    auto oldX = textScreenPos.x;
                    textScreenPos.x = (1.0f + std::floor((1.0f + textScreenPos.x - cursorScreenPos.x) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize) + cursorScreenPos.x;
                    ++i;

                    if (mTextStart + oldX < contentSize.x)
                        ++columnNo;
                }
                else
                {
                    auto d = UTF8CharLength(glyph.mChar);
                    char buf[7];
                    int j = 0;
                    while (j < d && i < (int)line.size())
                        buf[j++] = line[i++].mChar;
                    buf[j] = 0;
                    auto tsize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf, nullptr, nullptr);
                    drawList->AddText(textScreenPos, color, buf);
                    textScreenPos.x += tsize.x;
                    ++columnNo;
                }
            }

            ++lineNo;
        }
    }

    if (mScrollToCursor)
    {
        int lineNo = mState.mCursorPosition.mLine;
        auto lineHeight = ImGui::GetTextLineHeightWithSpacing();
        auto scrollY = ImGui::GetScrollY();

        if (lineNo * lineHeight < scrollY)
            SmoothScrollTo(std::max(0.0f, lineNo * lineHeight));
        if ((lineNo + 1) * lineHeight > scrollY + contentSize.y)
            SmoothScrollTo(std::max(0.0f, (lineNo + 1) * lineHeight - contentSize.y));

        mScrollToCursor = false;
    }

    if (mScrollToTop)
    {
        SmoothScrollTo(0.0f);
        mScrollToTop = false;
    }

    ImGui::Dummy(ImVec2(longest + 2, mLines.size() * ImGui::GetTextLineHeightWithSpacing()));

    ImGui::PopAllowKeyboardFocus();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);

    mWithinRender = false;
}

void TextEditor::SmoothScrollTo(float y)
{
    y = std::max(0.0f, y);
    mDesiredScrollY = y;
}

float TextEditor::TextDistanceToLineStart(const Coordinates& aFrom) const
{
    auto& line = mLines[aFrom.mLine];
    float distance = 0.0f;
    float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr).x;
    int colIndex = 0;
    for (size_t it = 0; it < line.size() && it < aFrom.mColumn;)
    {
        if (line[it].mChar == '\t')
        {
            distance = (1.0f + std::floor((1.0f + distance) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
            ++it;
        }
        else
        {
            auto d = UTF8CharLength(line[it].mChar);
            char tempCString[7];
            int i = 0;
            for (; i < d && it < (int)line.size() && i < 6; i++, it++)
                tempCString[i] = line[it].mChar;

            tempCString[i] = '\0';
            distance += ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, tempCString, nullptr, nullptr).x;
        }
        ++colIndex;
    }

    return distance;
}

TextEditor::Coordinates TextEditor::FindWordStart(const Coordinates& aFrom) const
{
    Coordinates at = aFrom;
    if (at.mLine >= (int)mLines.size())
        return at;

    auto& line = mLines[at.mLine];
    auto cindex = at.mColumn;

    if (cindex >= (int)line.size())
        return at;

    while (cindex > 0 && isspace(line[cindex].mChar))
        --cindex;

    auto cstart = (PaletteIndex)line[cindex].mColorIndex;
    while (cindex > 0)
    {
        auto c = line[cindex].mChar;
        if ((c & 0xC0) != 0x80)
        {
            if (cindex > 0 && isspace(line[cindex - 1].mChar))
                break;
        }
        --cindex;
    }

    return Coordinates(at.mLine, cindex);
}

TextEditor::Coordinates TextEditor::FindWordEnd(const Coordinates& aFrom) const
{
    Coordinates at = aFrom;
    if (at.mLine >= (int)mLines.size())
        return at;

    auto& line = mLines[at.mLine];
    auto cindex = at.mColumn;

    if (cindex >= (int)line.size())
        return at;

    while (cindex < (int)line.size() && isspace(line[cindex].mChar))
        ++cindex;

    auto cstart = (PaletteIndex)line[cindex].mColorIndex;
    while (cindex < (int)line.size())
    {
        auto c = line[cindex].mChar;
        if ((c & 0xC0) != 0x80)
        {
            if (cindex < (int)line.size() - 1 && isspace(line[cindex + 1].mChar))
            {
                ++cindex;
                break;
            }
        }
        ++cindex;
    }

    return Coordinates(at.mLine, cindex);
}

void TextEditor::DeleteRange(const Coordinates& aStart, const Coordinates& aEnd)
{
    if (aEnd <= aStart)
        return;

    auto start = aStart;
    auto end = aEnd;

    if (start.mLine == end.mLine)
    {
        auto& line = mLines[start.mLine];
        auto n = end.mColumn - start.mColumn;
        if (n > 0)
        {
            if (start.mColumn < (int)line.size())
            {
                n = std::min(n, (int)line.size() - start.mColumn);
                line.erase(line.begin() + start.mColumn, line.begin() + start.mColumn + n);
            }
        }
    }
    else
    {
        auto& firstLine = mLines[start.mLine];
        auto& lastLine = mLines[end.mLine];

        firstLine.erase(firstLine.begin() + start.mColumn, firstLine.end());
        lastLine.erase(lastLine.begin(), lastLine.begin() + end.mColumn);

        if (start.mLine < end.mLine)
            firstLine.insert(firstLine.end(), lastLine.begin(), lastLine.end());

        if (start.mLine < end.mLine)
            RemoveLine(start.mLine + 1, end.mLine + 1);
    }
}

