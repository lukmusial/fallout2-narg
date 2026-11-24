#ifndef TEXT_OBJECT_H
#define TEXT_OBJECT_H

#include <vector>

#include "color.h"
#include "geometry.h"
#include "obj_types.h"

namespace fallout {

int textObjectsInit(unsigned char* windowBuffer, int width, int height);
int textObjectsReset();
void textObjectsFree();
void textObjectsDisable();
void textObjectsEnable();
void textObjectsSetBaseDelay(double value);
void textObjectsSetLineDelay(double value);

int textObjectAdd(Object* object, char* string, int font, ColorWithFlags color, ColorWithFlags outlineColor, Rect* rect);

inline int textObjectAdd(Object* object, char* string, int font, Color color, Color outlineColor, Rect* rect)
{
    return textObjectAdd(object, string, font, color | DRAW_TEXT_FLAG_NONE, outlineColor | DRAW_TEXT_FLAG_NONE, rect);
}

void textObjectsRenderInRect(Rect* rect);
int textObjectsGetCount();
// CE: where each floating text is drawn (map window coordinates), for HD
// lights that must not take the text for a glowing sign.
void textObjectsGetRects(std::vector<Rect>* rects);
void textObjectsRemoveByOwner(Object* object);

} // namespace fallout

#endif /* TEXT_OBJECT_H */
