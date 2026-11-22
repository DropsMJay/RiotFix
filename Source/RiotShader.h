/*
 * RiotShader - the pixel shader the scene is drawn through
 *
 * Licence: MIT
 */
#ifndef RiotShaderHeader
#define RiotShaderHeader

/* Reads the shader settings of [Display] from RiotFix.ini and applies them to CurrentGame. The Suffering only:
 * every address here was measured in that build and none of them has been looked for in Ties That Bind. */
void RiotShaderApply(void);

#endif
