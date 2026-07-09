#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>

#include "cctk.h"
#include "util_Expression.h"
#include "util_String.h"

#include "eval.h"

static int TriggerEvaluator(int nvars, const char * const *vars, uExpressionValue *vals, const void *data);

 /*@@
   @routine    Trigger_EvaluateFormula
   @date       Thu Sep  6 17:08:46 EDT 2012
   @author     Roland Haas
   @desc
   Evaluates a formula using parameters and grid scalars as variables
   @enddesc

   @var     eq
   @vdesc   formula to evaluate
   @vtype   char *
   @vio     in
   @vcomment

   @endvar
   @var     cctkGH
   @vdesc   Cactus grid handle
   @vtype   const cGH *
   @vio     in
   @vcomment
   Used to access grid functions.
   @endvar
   @var     valtype
   @vdesc   Cactus variable type of returned value, CCTK_VARIABLE_REAL or CCTK_VARIABLE_INT
   @vtype   int
   @vio     in
   @vcomment

   @endvar
   @var     val
   @vdesc   Points to storage for returned value.
   @vtype   void *
   @vio     out
   @vcomment
   must point to either a CCTK_REAL or a CCTK_INT
   @endvar

   @returntype int
   @returndesc
   0  - for no error
   <0 - for errors, actual value is not well defined
   @endreturndesc
 @@*/
int Trigger_EvaluateFormula(const char * eq, const cGH *cctkGH, int valtype, void * val)
{
  // first we try just a simple atoi or atof to speed up constants
  assert(valtype == CCTK_VARIABLE_INT || valtype == CCTK_VARIABLE_REAL);

  int retval;
  const char *endptr;
  uExpressionValue eval;

  retval = 0;

  /* try parsing as number */
  if (valtype == CCTK_VARIABLE_INT) 
  {
    eval.value.ival = strtol (eq, (char**)&endptr, 0);
    eval.type = ival;
  }
  else
  {
    /*
     * Canonicalize the string by converting all exponent letters
     * (we allow [eEdD]) to 'e', since strtod(3) (which we will use
     * to do the actual conversion) only groks [eE].
     */
    char *temp = strdup (eq);
    for (unsigned int p = 0; p < strlen (temp); p++)
    {
      if (temp[p] == 'E' || temp[p] == 'd' || temp[p] == 'D')
      {
        temp[p] = 'e';
        break;
      }
    }

    eval.value.rval = strtod (temp, (char**)&endptr);
    eval.type = rval;

    endptr = eq + (endptr-temp); // transfer endptr offset to eq since temp is freed
    free(temp);
  }

  if (*endptr) /* if we could not parse as a number, try an expression */
  {
    uExpression *expr;

    expr = Util_ExpressionParse(eq);
    if (expr != NULL)
    {
      retval = Util_ExpressionEvaluate(expr, &eval, TriggerEvaluator, (void*)cctkGH);
      Util_ExpressionFree(expr);
    }
    else
    {
      retval = -6;
    }
  }

  if (retval == 0)
  {
    assert(eval.type == ival || eval.type == rval);

    if (valtype == CCTK_VARIABLE_INT)
    {
      if(eval.type == ival)
      {
        *(CCTK_INT*)val =(CCTK_INT)eval.value.ival;
      }
      else if(fabs(round(eval.value.rval) - eval.value.rval) < 1e-12) /* enforce integer result */
      {
        double rinval = round(eval.value.rval);
        *(CCTK_INT*)val = (CCTK_INT)rinval;
      }
      else
      {
        retval = -6;
      }
    }
    else
    {
      if(eval.type == ival)
      {
        *(CCTK_REAL*)val =(CCTK_REAL)eval.value.ival;
      }
      else
      {
        *(CCTK_REAL*)val =(CCTK_REAL)eval.value.rval;
      }
    }
  }

  return retval;
}

 /*@@
   @routine    TriggerEvaluator
   @date       Thu Sep  6 17:08:46 EDT 2012
   @author     Roland Haas
   @desc
   Routine called from the expression parser to evaluate
   the vars in an parameter expression
   @enddesc

   @var     nvars
   @vdesc   Number of variables to evaluate
   @vtype   int
   @vio     in
   @vcomment

   @endvar
   @var     vars
   @vdesc   an array of variable names
   @vtype   const char * const *
   @vio     in
   @vcomment
   Either a fully qualified parameter or grid scalar name or a number.
   @endvar
   @var     vals
   @vdesc   Output array to hold values
   @vtype   uExpressionValue *
   @vio     out
   @vcomment

   @endvar
   @var     data
   @vdesc   Data passed from expression evaluator
   @vtype   void *
   @vio     in
   @vcomment
   Not used.
   @endvar

   @returntype int
   @returndesc
   0
   @endreturndesc
 @@*/
static int TriggerEvaluator(int nvars, const char * const *vars, uExpressionValue *vals, const void *data)
{
  const cGH *cctkGH = (const cGH *)data;
  int retval = 0;

  for (int i=0; i < nvars; i++)
  {
    int ierr = 0;

    if (strstr(vars[i], "::")) /* a grid variable or parameter */
    {
      const void *paramval;
      int type, varindex;
      char *name, *thorn;

      if (Util_SplitString(&thorn, &name, vars[i], "::") == 0)
      {
        paramval = CCTK_ParameterGet(name, thorn, &type);
        if (paramval)
        {
          switch(type)
          {
            case PARAMETER_REAL:
              vals[i].type = rval;
              vals[i].value.rval = *(const CCTK_REAL *)paramval;
              ierr = 0;
              break;
            case PARAMETER_INT:
              vals[i].type = ival;
              vals[i].value.ival = *(const CCTK_INT *)paramval;
              ierr = 0;
              break;
            case PARAMETER_BOOLEAN:
              vals[i].type = ival;
              vals[i].value.ival = *(const CCTK_INT *)paramval;
              ierr = 0;
              break;
            default:
              CCTK_VWarn (0, __LINE__, __FILE__, "Cactus",
                          "TriggerEvaluator: cannot handle type %d for parameter '%s::%s'. Only REAL, INT and BOOLEAN are supported.",
                          type, thorn, name);
              ierr = -1;
              break;
          }
        }
        else if ((varindex = CCTK_VarIndex(vars[i])) >= 0 && (paramval = CCTK_VarDataPtrI(cctkGH,0,varindex)) != NULL)
        {
          type = CCTK_VarTypeI(varindex);
          switch(type)
          {
            case CCTK_VARIABLE_REAL:
              vals[i].type = rval;
              vals[i].value.rval = *(const CCTK_REAL *)paramval;
              ierr = 0;
              break;
            case CCTK_VARIABLE_INT:
              vals[i].type = ival;
              vals[i].value.ival = *(const CCTK_INT *)paramval;
              ierr = 0;
              break;
            default:
              CCTK_VWarn (0, __LINE__, __FILE__, "Cactus",
                          "TriggerEvaluator: cannot handle type %d for variable '%s::%s'. Only REAL and INT are supported.",
                          type, thorn, name);
              ierr = -1;
              break;
          }
        } 
        else
        {
          CCTK_VWarn (2, __LINE__, __FILE__, "Cactus",
                      "TriggerEvaluator: could not find '%s::%s'",
                      thorn, name);
          ierr = -1;
        }

        free(thorn);
        free(name);
      }
      else
      {
        CCTK_VWarn (2, __LINE__, __FILE__, "Cactus",
                    "SetVarEvaluator: cannot split '%s' into thorn::parameter: %d",
                    vars[i], ierr);
        ierr = -1;
      }
    }
    else /* a direct value */
    {
      char *endptr, *temp;
      if (strpbrk(vars[i], "eDdD.") ||
          Util_StrCmpi(vars[i], "nan") == 0 ||
          Util_StrCmpi(vars[i], "inf") == 0)
      {
        /*
         * Canonicalize the string by converting all exponent letters
         * (we allow [eEdD]) to 'e', since strtod(3) (which we will use
         * to do the actual conversion) only groks [eE].
         */
        temp = strdup (vars[i]);
        for (unsigned int p = 0; p < strlen (temp); p++)
        {
          if (temp[p] == 'E' || temp[p] == 'd' || temp[p] == 'D')
          {
            temp[p] = 'e';
            break;
          }
        }

        vals[i].type = rval;
        vals[i].value.rval = (CCTK_REAL)strtod (temp, &endptr);
        if (*endptr)
          ierr = -1;

        free(temp);
      }
      else
      {
          vals[i].type = ival;
          vals[i].value.ival = (CCTK_INT)strtol (vars[i], &endptr, 0);
          if (*endptr)
            ierr = -1;
      }
    }

    if (ierr)
    {
      CCTK_VWarn (2, __LINE__, __FILE__, "Cactus",
                  "TriggerEvaluator: Unable to set value - '%s' "
                  "does not evaluate to a real or integer or boolean",
                  vars[i]);
      vals[i].type = rval;
      vals[i].value.rval = (double)atof("nan");
    }

    retval += ierr;
  }

  return retval;
}
