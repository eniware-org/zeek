// See the file "COPYING" in the main distribution directory for copyright.

#include "zeek-config.h"

#include "Expr.h"
#include "Event.h"
#include "Frame.h"
#include "Func.h"
#include "RE.h"
#include "Scope.h"
#include "Stmt.h"
#include "EventRegistry.h"
#include "Net.h"
#include "Traverse.h"
#include "Trigger.h"
#include "IPAddr.h"
#include "digest.h"

#include "broker/Data.h"

const char* expr_name(BroExprTag t)
	{
	static const char* expr_names[int(NUM_EXPRS)] = {
		"name", "const",
		"(*)",
		"++", "--", "!", "~", "+", "-",
		"+", "-", "+=", "-=", "*", "/", "%",
		"&", "|", "^",
		"&&", "||",
		"<", "<=", "==", "!=", ">=", ">", "?:", "ref",
		"=", "[]", "$", "?$", "[=]",
		"table()", "set()", "vector()",
		"$=", "in", "<<>>",
		"()", "function()", "event", "schedule",
		"coerce", "record_coerce", "table_coerce",
		"sizeof", "flatten", "cast", "is", "[:]="
	};

	if ( int(t) >= NUM_EXPRS )
		{
		static char errbuf[512];

		// This isn't quite right - we return a static buffer,
		// so multiple calls to expr_name() could lead to confusion
		// by overwriting the buffer.  But oh well.
		snprintf(errbuf, sizeof(errbuf),
				"%d: not an expression tag", int(t));
		return errbuf;
		}

	return expr_names[int(t)];
	}

Expr::Expr(BroExprTag arg_tag)
	{
	tag = arg_tag;
	type = 0;
	paren = 0;

	SetLocationInfo(&start_location, &end_location);
	}

Expr::~Expr()
	{
	Unref(type);
	}

int Expr::CanAdd() const
	{
	return 0;
	}

int Expr::CanDel() const
	{
	return 0;
	}

void Expr::Add(Frame* /* f */)
	{
	Internal("Expr::Delete called");
	}

void Expr::Delete(Frame* /* f */)
	{
	Internal("Expr::Delete called");
	}

Expr* Expr::MakeLvalue()
	{
	if ( ! IsError() )
		ExprError("can't be assigned to");
	return this;
	}

void Expr::EvalIntoAggregate(const BroType* /* t */, Val* /* aggr */,
				Frame* /* f */) const
	{
	Internal("Expr::EvalIntoAggregate called");
	}

void Expr::Assign(Frame* /* f */, Val* /* v */)
	{
	Internal("Expr::Assign called");
	}

BroType* Expr::InitType() const
	{
	return type->Ref();
	}

int Expr::IsRecordElement(TypeDecl* /* td */) const
	{
	return 0;
	}

int Expr::IsPure() const
	{
	return 1;
	}

Val* Expr::InitVal(const BroType* t, Val* aggr) const
	{
	if ( aggr )
		{
		Error("bad initializer");
		return 0;
		}

	if ( IsError() )
		return 0;

	return check_and_promote(Eval(0), t, 1);
	}

void Expr::SetError(const char* msg)
	{
	Error(msg);
	SetError();
	}

void Expr::Describe(ODesc* d) const
	{
	if ( IsParen() && ! d->IsBinary() )
		d->Add("(");

	if ( d->IsPortable() || d->IsBinary() )
		AddTag(d);

	ExprDescribe(d);

	if ( IsParen() && ! d->IsBinary() )
		d->Add(")");
	}

void Expr::AddTag(ODesc* d) const
	{
	if ( d->IsBinary() )
		d->Add(int(Tag()));
	else
		d->AddSP(expr_name(Tag()));
	}

void Expr::Canonicize()
	{
	}

void Expr::SetType(BroType* t)
	{
	if ( ! type || type->Tag() != TYPE_ERROR )
		{
		Unref(type);
		type = t;
		}
	else
		Unref(t);
	}

void Expr::ExprError(const char msg[])
	{
	Error(msg);
	SetError();
	}

void Expr::RuntimeError(const std::string& msg) const
	{
	reporter->ExprRuntimeError(this, "%s", msg.data());
	}

void Expr::RuntimeErrorWithCallStack(const std::string& msg) const
	{
	auto rcs = render_call_stack();

	if ( rcs.empty() )
		reporter->ExprRuntimeError(this, "%s", msg.data());
	else
		{
		ODesc d;
		d.SetShort();
		Describe(&d);
		reporter->RuntimeError(GetLocationInfo(), "%s, expression: %s, call stack: %s",
		                       msg.data(), d.Description(), rcs.data());
		}
	}

NameExpr::NameExpr(ID* arg_id, bool const_init) : Expr(EXPR_NAME)
	{
	id = arg_id;
	in_const_init = const_init;

	if ( id->AsType() )
		SetType(new TypeType(id->AsType()));
	else
		SetType(id->Type()->Ref());

	EventHandler* h = event_registry->Lookup(id->Name());
	if ( h )
		h->SetUsed();
	}

NameExpr::~NameExpr()
	{
	Unref(id);
	}

Val* NameExpr::Eval(Frame* f) const
	{
	Val* v;

	if ( id->AsType() )
		return new Val(id->AsType(), true);

	if ( id->IsGlobal() )
		v = id->ID_Val();

	else if ( f )
		v = f->GetElement(id);

	else
		// No frame - evaluating for Simplify() purposes
		return 0;

	if ( v )
		return v->Ref();
	else
		{
		RuntimeError("value used but not set");
		return 0;
		}
	}

Expr* NameExpr::MakeLvalue()
	{
	if ( id->AsType() )
		ExprError("Type name is not an lvalue");

	if ( id->IsConst() && ! in_const_init )
		ExprError("const is not a modifiable lvalue");

	if ( id->IsOption() && ! in_const_init )
		ExprError("option is not a modifiable lvalue");

	return new RefExpr(this);
	}

void NameExpr::Assign(Frame* f, Val* v)
	{
	if ( id->IsGlobal() )
		id->SetVal(v);
	else
		f->SetElement(id, v);
	}

int NameExpr::IsPure() const
	{
	return id->IsConst();
	}

TraversalCode NameExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = id->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}


void NameExpr::ExprDescribe(ODesc* d) const
	{
	if ( d->IsReadable() )
		d->Add(id->Name());
	else
		{
		if ( d->IsPortable() )
			d->Add(id->Name());
		else
			d->AddCS(id->Name());
		}
	}

ConstExpr::ConstExpr(Val* arg_val) : Expr(EXPR_CONST)
	{
	val = arg_val;

	if ( val->Type()->Tag() == TYPE_LIST && val->AsListVal()->Length() == 1 )
		{
		val = val->AsListVal()->Index(0);
		val->Ref();
		Unref(arg_val);
		}

	SetType(val->Type()->Ref());
	}

ConstExpr::~ConstExpr()
	{
	Unref(val);
	}

void ConstExpr::ExprDescribe(ODesc* d) const
	{
	val->Describe(d);
	}

Val* ConstExpr::Eval(Frame* /* f */) const
	{
	return Value()->Ref();
	}

TraversalCode ConstExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

UnaryExpr::UnaryExpr(BroExprTag arg_tag, Expr* arg_op) : Expr(arg_tag)
	{
	op = arg_op;
	if ( op->IsError() )
		SetError();
	}

UnaryExpr::~UnaryExpr()
	{
	Unref(op);
	}

Val* UnaryExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return 0;

	Val* v = op->Eval(f);

	if ( ! v )
		return 0;

	if ( is_vector(v) && Tag() != EXPR_IS && Tag() != EXPR_CAST )
		{
		VectorVal* v_op = v->AsVectorVal();
		VectorType* out_t;
		if ( Type()->Tag() == TYPE_ANY )
			out_t = v->Type()->AsVectorType();
		else
			out_t = Type()->AsVectorType();

		VectorVal* result = new VectorVal(out_t);

		for ( unsigned int i = 0; i < v_op->Size(); ++i )
			{
			Val* v_i = v_op->Lookup(i);
			result->Assign(i, v_i ? Fold(v_i) : 0);
			}

		Unref(v);
		return result;
		}
	else
		{
		Val* result = Fold(v);
		Unref(v);
		return result;
		}
	}

int UnaryExpr::IsPure() const
	{
	return op->IsPure();
	}

TraversalCode UnaryExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

Val* UnaryExpr::Fold(Val* v) const
	{
	return v->Ref();
	}

void UnaryExpr::ExprDescribe(ODesc* d) const
	{
	bool is_coerce =
		Tag() == EXPR_ARITH_COERCE || Tag() == EXPR_RECORD_COERCE ||
		Tag() == EXPR_TABLE_COERCE;

	if ( d->IsReadable() )
		{
		if ( is_coerce )
			d->Add("(coerce ");
		else if ( Tag() == EXPR_FLATTEN )
			d->Add("flatten ");
		else if ( Tag() != EXPR_REF )
			d->Add(expr_name(Tag()));
		}

	op->Describe(d);

	if ( d->IsReadable() && is_coerce )
		{
		d->Add(" to ");
		Type()->Describe(d);
		d->Add(")");
		}
	}

BinaryExpr::~BinaryExpr()
	{
	Unref(op1);
	Unref(op2);
	}

Val* BinaryExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return 0;

	Val* v1 = op1->Eval(f);
	if ( ! v1 )
		return 0;

	Val* v2 = op2->Eval(f);
	if ( ! v2 )
		{
		Unref(v1);
		return 0;
		}

	Val* result = 0;

	int is_vec1 = is_vector(v1);
	int is_vec2 = is_vector(v2);

	if ( is_vec1 && is_vec2 )
		{ // fold pairs of elements
		VectorVal* v_op1 = v1->AsVectorVal();
		VectorVal* v_op2 = v2->AsVectorVal();

		if ( v_op1->Size() != v_op2->Size() )
			{
			RuntimeError("vector operands are of different sizes");
			return 0;
			}

		VectorVal* v_result = new VectorVal(Type()->AsVectorType());

		for ( unsigned int i = 0; i < v_op1->Size(); ++i )
			{
			if ( v_op1->Lookup(i) && v_op2->Lookup(i) )
				v_result->Assign(i,
						 Fold(v_op1->Lookup(i),
						      v_op2->Lookup(i)));
			else
				v_result->Assign(i, 0);
			// SetError("undefined element in vector operation");
			}

		Unref(v1);
		Unref(v2);
		return v_result;
		}

	if ( IsVector(Type()->Tag()) && (is_vec1 || is_vec2) )
		{ // fold vector against scalar
		VectorVal* vv = (is_vec1 ? v1 : v2)->AsVectorVal();
		VectorVal* v_result = new VectorVal(Type()->AsVectorType());

		for ( unsigned int i = 0; i < vv->Size(); ++i )
			{
			Val* vv_i = vv->Lookup(i);
			if ( vv_i )
				v_result->Assign(i,
					 is_vec1 ?
						 Fold(vv_i, v2) : Fold(v1, vv_i));
			else
				v_result->Assign(i, 0);

			// SetError("Undefined element in vector operation");
			}

		Unref(v1);
		Unref(v2);
		return v_result;
		}

	// scalar op scalar
	result = Fold(v1, v2);

	Unref(v1);
	Unref(v2);
	return result;
	}

int BinaryExpr::IsPure() const
	{
	return op1->IsPure() && op2->IsPure();
	}

TraversalCode BinaryExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op1->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op2->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void BinaryExpr::ExprDescribe(ODesc* d) const
	{
	op1->Describe(d);

	d->SP();
	if ( d->IsReadable() )
		d->AddSP(expr_name(Tag()));

	op2->Describe(d);
	}

Val* BinaryExpr::Fold(Val* v1, Val* v2) const
	{
	InternalTypeTag it = v1->Type()->InternalType();

	if ( it == TYPE_INTERNAL_STRING )
		return StringFold(v1, v2);

	if ( v1->Type()->Tag() == TYPE_PATTERN )
		return PatternFold(v1, v2);

	if ( v1->Type()->IsSet() )
		return SetFold(v1, v2);

	if ( it == TYPE_INTERNAL_ADDR )
		return AddrFold(v1, v2);

	if ( it == TYPE_INTERNAL_SUBNET )
		return SubNetFold(v1, v2);

	bro_int_t i1 = 0, i2 = 0, i3 = 0;
	bro_uint_t u1 = 0, u2 = 0, u3 = 0;
	double d1 = 0.0, d2 = 0.0, d3 = 0.0;
	int is_integral = 0;
	int is_unsigned = 0;

	if ( it == TYPE_INTERNAL_INT )
		{
		i1 = v1->InternalInt();
		i2 = v2->InternalInt();
		++is_integral;
		}
	else if ( it == TYPE_INTERNAL_UNSIGNED )
		{
		u1 = v1->InternalUnsigned();
		u2 = v2->InternalUnsigned();
		++is_unsigned;
		}
	else if ( it == TYPE_INTERNAL_DOUBLE )
		{
		d1 = v1->InternalDouble();
		d2 = v2->InternalDouble();
		}
	else
		RuntimeErrorWithCallStack("bad type in BinaryExpr::Fold");

	switch ( tag ) {
#define DO_INT_FOLD(op) \
	if ( is_integral ) \
		i3 = i1 op i2; \
	else if ( is_unsigned ) \
		u3 = u1 op u2; \
	else \
		RuntimeErrorWithCallStack("bad type in BinaryExpr::Fold");

#define DO_UINT_FOLD(op) \
	if ( is_unsigned ) \
		u3 = u1 op u2; \
	else \
		RuntimeErrorWithCallStack("bad type in BinaryExpr::Fold");

#define DO_FOLD(op) \
	if ( is_integral ) \
		i3 = i1 op i2; \
	else if ( is_unsigned ) \
		u3 = u1 op u2; \
	else \
		d3 = d1 op d2;

#define DO_INT_VAL_FOLD(op) \
	if ( is_integral ) \
		i3 = i1 op i2; \
	else if ( is_unsigned ) \
		i3 = u1 op u2; \
	else \
		i3 = d1 op d2;

	case EXPR_ADD:		DO_FOLD(+); break;
	case EXPR_ADD_TO:	DO_FOLD(+); break;
	case EXPR_SUB:		DO_FOLD(-); break;
	case EXPR_REMOVE_FROM:	DO_FOLD(-); break;
	case EXPR_TIMES:	DO_FOLD(*); break;
	case EXPR_DIVIDE:
		{
		if ( is_integral )
			{
			if ( i2 == 0 )
				RuntimeError("division by zero");

			i3 = i1 / i2;
			}

		else if ( is_unsigned )
			{
			if ( u2 == 0 )
				RuntimeError("division by zero");

			u3 = u1 / u2;
			}
		else
			{
			if ( d2 == 0 )
				RuntimeError("division by zero");

			d3 = d1 / d2;
			}

		}
		break;

	case EXPR_MOD:
		{
		if ( is_integral )
			{
			if ( i2 == 0 )
				RuntimeError("modulo by zero");

			i3 = i1 % i2;
			}

		else if ( is_unsigned )
			{
			if ( u2 == 0 )
				RuntimeError("modulo by zero");

			u3 = u1 % u2;
			}

		else
			RuntimeErrorWithCallStack("bad type in BinaryExpr::Fold");
		}

		break;

	case EXPR_AND:		DO_UINT_FOLD(&); break;
	case EXPR_OR:		DO_UINT_FOLD(|); break;
	case EXPR_XOR:		DO_UINT_FOLD(^); break;

	case EXPR_AND_AND:	DO_INT_FOLD(&&); break;
	case EXPR_OR_OR:	DO_INT_FOLD(||); break;

	case EXPR_LT:		DO_INT_VAL_FOLD(<); break;
	case EXPR_LE:		DO_INT_VAL_FOLD(<=); break;
	case EXPR_EQ:		DO_INT_VAL_FOLD(==); break;
	case EXPR_NE:		DO_INT_VAL_FOLD(!=); break;
	case EXPR_GE:		DO_INT_VAL_FOLD(>=); break;
	case EXPR_GT:		DO_INT_VAL_FOLD(>); break;

	default:
		BadTag("BinaryExpr::Fold", expr_name(tag));
	}

	BroType* ret_type = type;
	if ( IsVector(ret_type->Tag()) )
	     ret_type = ret_type->YieldType();

	if ( ret_type->Tag() == TYPE_INTERVAL )
		return new IntervalVal(d3, 1.0);
	else if ( ret_type->InternalType() == TYPE_INTERNAL_DOUBLE )
		return new Val(d3, ret_type->Tag());
	else if ( ret_type->InternalType() == TYPE_INTERNAL_UNSIGNED )
		return val_mgr->GetCount(u3);
	else if ( ret_type->Tag() == TYPE_BOOL )
		return val_mgr->GetBool(i3);
	else
		return val_mgr->GetInt(i3);
	}

Val* BinaryExpr::StringFold(Val* v1, Val* v2) const
	{
	const BroString* s1 = v1->AsString();
	const BroString* s2 = v2->AsString();
	int result = 0;

	switch ( tag ) {
#undef DO_FOLD
#define DO_FOLD(sense) { result = Bstr_cmp(s1, s2) sense 0; break; }

	case EXPR_LT:		DO_FOLD(<)
	case EXPR_LE:		DO_FOLD(<=)
	case EXPR_EQ:		DO_FOLD(==)
	case EXPR_NE:		DO_FOLD(!=)
	case EXPR_GE:		DO_FOLD(>=)
	case EXPR_GT:		DO_FOLD(>)

	case EXPR_ADD:
	case EXPR_ADD_TO:
		{
		vector<const BroString*> strings;
		strings.push_back(s1);
		strings.push_back(s2);

		return new StringVal(concatenate(strings));
		}

	default:
		BadTag("BinaryExpr::StringFold", expr_name(tag));
	}

	return val_mgr->GetBool(result);
	}


Val* BinaryExpr::PatternFold(Val* v1, Val* v2) const
	{
	const RE_Matcher* re1 = v1->AsPattern();
	const RE_Matcher* re2 = v2->AsPattern();

	if ( tag != EXPR_AND && tag != EXPR_OR )
		BadTag("BinaryExpr::PatternFold");

	RE_Matcher* res = tag == EXPR_AND ?
		RE_Matcher_conjunction(re1, re2) :
		RE_Matcher_disjunction(re1, re2);

	return new PatternVal(res);
	}

Val* BinaryExpr::SetFold(Val* v1, Val* v2) const
	{
	TableVal* tv1 = v1->AsTableVal();
	TableVal* tv2 = v2->AsTableVal();
	TableVal* result;
	bool res = false;

	switch ( tag ) {
	case EXPR_AND:
		return tv1->Intersect(tv2);

	case EXPR_OR:
		result = v1->Clone()->AsTableVal();

		if ( ! tv2->AddTo(result, false, false) )
			reporter->InternalError("set union failed to type check");
		return result;

	case EXPR_SUB:
		result = v1->Clone()->AsTableVal();

		if ( ! tv2->RemoveFrom(result) )
			reporter->InternalError("set difference failed to type check");
		return result;

	case EXPR_EQ:
		res = tv1->EqualTo(tv2);
		break;

	case EXPR_NE:
		res = ! tv1->EqualTo(tv2);
		break;

	case EXPR_LT:
		res = tv1->IsSubsetOf(tv2) && tv1->Size() < tv2->Size();
		break;

	case EXPR_LE:
		res = tv1->IsSubsetOf(tv2);
		break;

	case EXPR_GE:
	case EXPR_GT:
		// These should't happen due to canonicalization.
		reporter->InternalError("confusion over canonicalization in set comparison");
		break;

	default:
		BadTag("BinaryExpr::SetFold", expr_name(tag));
		return 0;
	}

	return val_mgr->GetBool(res);
	}

Val* BinaryExpr::AddrFold(Val* v1, Val* v2) const
	{
	IPAddr a1 = v1->AsAddr();
	IPAddr a2 = v2->AsAddr();
	int result = 0;

	switch ( tag ) {

	case EXPR_LT:
		result = a1 < a2;
		break;
	case EXPR_LE:
		result = a1 < a2 || a1 == a2;
		break;
	case EXPR_EQ:
		result = a1 == a2;
		break;
	case EXPR_NE:
		result = a1 != a2;
		break;
	case EXPR_GE:
		result = ! ( a1 < a2 );
		break;
	case EXPR_GT:
		result = ( ! ( a1 < a2 ) ) && ( a1 != a2 );
		break;

	default:
		BadTag("BinaryExpr::AddrFold", expr_name(tag));
	}

	return val_mgr->GetBool(result);
	}

Val* BinaryExpr::SubNetFold(Val* v1, Val* v2) const
	{
	const IPPrefix& n1 = v1->AsSubNet();
	const IPPrefix& n2 = v2->AsSubNet();

	bool result = ( n1 == n2 ) ? true : false;

	if ( tag == EXPR_NE )
		result = ! result;

	return val_mgr->GetBool(result);
	}

void BinaryExpr::SwapOps()
	{
	// We could check here whether the operator is commutative.
	Expr* t = op1;
	op1 = op2;
	op2 = t;
	}

void BinaryExpr::PromoteOps(TypeTag t)
	{
	TypeTag bt1 = op1->Type()->Tag();
	TypeTag bt2 = op2->Type()->Tag();

	bool is_vec1 = IsVector(bt1);
	bool is_vec2 = IsVector(bt2);

	if ( is_vec1 )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();
	if ( is_vec2 )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	if ( (is_vec1 || is_vec2) && ! (is_vec1 && is_vec2) )
		reporter->Warning("mixing vector and scalar operands is deprecated");

	if ( bt1 != t )
		op1 = new ArithCoerceExpr(op1, t);
	if ( bt2 != t )
		op2 = new ArithCoerceExpr(op2, t);
	}

void BinaryExpr::PromoteType(TypeTag t, bool is_vector)
	{
	PromoteOps(t);
	SetType(is_vector ? new VectorType(base_type(t)) : base_type(t));
	}

CloneExpr::CloneExpr(Expr* arg_op) : UnaryExpr(EXPR_CLONE, arg_op)
	{
	if ( IsError() )
		return;

	BroType* t = op->Type();
	SetType(t->Ref());
	}

Val* CloneExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return 0;

	Val* v = op->Eval(f);

	if ( ! v )
		return 0;

	Val* result = Fold(v);
	Unref(v);

	return result;
	}

Val* CloneExpr::Fold(Val* v) const
	{
	return v->Clone();
	}

IncrExpr::IncrExpr(BroExprTag arg_tag, Expr* arg_op)
: UnaryExpr(arg_tag, arg_op->MakeLvalue())
	{
	if ( IsError() )
		return;

	BroType* t = op->Type();

	if ( IsVector(t->Tag()) )
		{
		if ( ! IsIntegral(t->AsVectorType()->YieldType()->Tag()) )
			ExprError("vector elements must be integral for increment operator");
		else
			{
			reporter->Warning("increment/decrement operations for vectors deprecated");
			SetType(t->Ref());
			}
		}
	else
		{
		if ( ! IsIntegral(t->Tag()) )
			ExprError("requires an integral operand");
		else
			SetType(t->Ref());
		}
	}

Val* IncrExpr::DoSingleEval(Frame* f, Val* v) const
	 {
	bro_int_t k = v->CoerceToInt();

	if ( Tag() == EXPR_INCR )
		++k;
	else
		{
		--k;

		if ( k < 0 &&
		     v->Type()->InternalType() == TYPE_INTERNAL_UNSIGNED )
			RuntimeError("count underflow");
		}

	 BroType* ret_type = Type();
	 if ( IsVector(ret_type->Tag()) )
		 ret_type = Type()->YieldType();

	if ( ret_type->Tag() == TYPE_INT )
		return val_mgr->GetInt(k);
	else
		return val_mgr->GetCount(k);
	 }


Val* IncrExpr::Eval(Frame* f) const
	{
	Val* v = op->Eval(f);
	if ( ! v )
		return 0;

	if ( is_vector(v) )
		{
		VectorVal* v_vec = v->AsVectorVal();
		for ( unsigned int i = 0; i < v_vec->Size(); ++i )
			{
			Val* elt = v_vec->Lookup(i);
			if ( elt )
				{
				Val* new_elt = DoSingleEval(f, elt);
				v_vec->Assign(i, new_elt);
				}
			else
				v_vec->Assign(i, 0);
			}
		op->Assign(f, v_vec);
		}

	else
		{
		Val* old_v = v;
		op->Assign(f, v = DoSingleEval(f, old_v));
		Unref(old_v);
		}

	return v->Ref();
	}

int IncrExpr::IsPure() const
	{
	return 0;
	}

ComplementExpr::ComplementExpr(Expr* arg_op) : UnaryExpr(EXPR_COMPLEMENT, arg_op)
	{
	if ( IsError() )
		return;

	BroType* t = op->Type();
	TypeTag bt = t->Tag();

	if ( bt != TYPE_COUNT )
		ExprError("requires \"count\" operand");
	else
		SetType(base_type(TYPE_COUNT));
	}

Val* ComplementExpr::Fold(Val* v) const
	{
	return val_mgr->GetCount(~ v->InternalUnsigned());
	}

NotExpr::NotExpr(Expr* arg_op) : UnaryExpr(EXPR_NOT, arg_op)
	{
	if ( IsError() )
		return;

	BroType* t = op->Type();
	TypeTag bt = t->Tag();

	if ( ! IsIntegral(bt) && bt != TYPE_BOOL )
		ExprError("requires an integral or boolean operand");
	else
		SetType(base_type(TYPE_BOOL));
	}

Val* NotExpr::Fold(Val* v) const
	{
	return val_mgr->GetBool(! v->InternalInt());
	}

PosExpr::PosExpr(Expr* arg_op) : UnaryExpr(EXPR_POSITIVE, arg_op)
	{
	if ( IsError() )
		return;

	BroType* t = op->Type();
	if ( IsVector(t->Tag()) )
		t = t->AsVectorType()->YieldType();
	TypeTag bt = t->Tag();

	BroType* base_result_type = 0;

	if ( IsIntegral(bt) )
		// Promote count and counter to int.
		base_result_type = base_type(TYPE_INT);
	else if ( bt == TYPE_INTERVAL || bt == TYPE_DOUBLE )
		base_result_type = t->Ref();
	else
		ExprError("requires an integral or double operand");

	if ( is_vector(op) )
		SetType(new VectorType(base_result_type));
	else
		SetType(base_result_type);
	}

Val* PosExpr::Fold(Val* v) const
	{
	TypeTag t = v->Type()->Tag();

	if ( t == TYPE_DOUBLE || t == TYPE_INTERVAL || t == TYPE_INT )
		return v->Ref();
	else
		return val_mgr->GetInt(v->CoerceToInt());
	}

NegExpr::NegExpr(Expr* arg_op) : UnaryExpr(EXPR_NEGATE, arg_op)
	{
	if ( IsError() )
		return;

	BroType* t = op->Type();
	if ( IsVector(t->Tag()) )
		t = t->AsVectorType()->YieldType();
	TypeTag bt = t->Tag();

	BroType* base_result_type = 0;

	if ( IsIntegral(bt) )
		// Promote count and counter to int.
		base_result_type = base_type(TYPE_INT);
	else if ( bt == TYPE_INTERVAL || bt == TYPE_DOUBLE )
		base_result_type = t->Ref();
	else
		ExprError("requires an integral or double operand");

	if ( is_vector(op) )
		SetType(new VectorType(base_result_type));
	else
		SetType(base_result_type);
	}

Val* NegExpr::Fold(Val* v) const
	{
	if ( v->Type()->Tag() == TYPE_DOUBLE )
		return new Val(- v->InternalDouble(), v->Type()->Tag());
	else if ( v->Type()->Tag() == TYPE_INTERVAL )
		return new IntervalVal(- v->InternalDouble(), 1.0);
	else
		return val_mgr->GetInt(- v->CoerceToInt());
	}

SizeExpr::SizeExpr(Expr* arg_op) : UnaryExpr(EXPR_SIZE, arg_op)
	{
	if ( IsError() )
		return;

	if ( op->Type()->InternalType() == TYPE_INTERNAL_DOUBLE )
		SetType(base_type(TYPE_DOUBLE));
	else
		SetType(base_type(TYPE_COUNT));
	}

Val* SizeExpr::Eval(Frame* f) const
	{
	Val* v = op->Eval(f);
	if ( ! v )
		return 0;

	Val* result = Fold(v);
	Unref(v);
	return result;
	}

Val* SizeExpr::Fold(Val* v) const
	{
	return v->SizeVal();
	}

AddExpr::AddExpr(Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(EXPR_ADD, arg_op1, arg_op2)
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();
	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = op2->Type()->Tag();
	if ( IsVector(bt2) )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	BroType* base_result_type = 0;

	if ( bt1 == TYPE_TIME && bt2 == TYPE_INTERVAL )
		base_result_type = base_type(bt1);
	else if ( bt2 == TYPE_TIME && bt1 == TYPE_INTERVAL )
		base_result_type = base_type(bt2);
	else if ( bt1 == TYPE_INTERVAL && bt2 == TYPE_INTERVAL )
		base_result_type = base_type(bt1);
	else if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1) || is_vector(op2));
	else if ( BothString(bt1, bt2) )
		base_result_type = base_type(bt1);
	else
		ExprError("requires arithmetic operands");

	if ( base_result_type )
		{
		if ( is_vector(op1) || is_vector(op2) )
			SetType(new VectorType(base_result_type));
		else
			SetType(base_result_type);
		}
	}

void AddExpr::Canonicize()
	{
	if ( expr_greater(op2, op1) ||
	     (op1->Type()->Tag() == TYPE_INTERVAL &&
	      op2->Type()->Tag() == TYPE_TIME) ||
	     (op2->IsConst() && ! is_vector(op2->ExprVal()) && ! op1->IsConst()))
		SwapOps();
	}

AddToExpr::AddToExpr(Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(EXPR_ADD_TO,
             is_vector(arg_op1) ? arg_op1 : arg_op1->MakeLvalue(), arg_op2)
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();
	TypeTag bt2 = op2->Type()->Tag();

	if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1) || is_vector(op2));
	else if ( BothString(bt1, bt2) )
		SetType(base_type(bt1));
	else if ( BothInterval(bt1, bt2) )
		SetType(base_type(bt1));

	else if ( IsVector(bt1) )
		{
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

		if ( IsArithmetic(bt1) )
			{
			if ( IsArithmetic(bt2) )
				{
				if ( bt2 != bt1 )
					op2 = new ArithCoerceExpr(op2, bt1);

				SetType(op1->Type()->Ref());
				}

			else
				ExprError("appending non-arithmetic to arithmetic vector");
			}

		else if ( bt1 != bt2 && bt1 != TYPE_ANY )
			ExprError(fmt("incompatible vector append: %s and %s",
					  type_name(bt1), type_name(bt2)));

		else
			SetType(op1->Type()->Ref());
		}

	else
		ExprError("requires two arithmetic or two string operands");
	}

Val* AddToExpr::Eval(Frame* f) const
	{
	Val* v1 = op1->Eval(f);
	if ( ! v1 )
		return 0;

	Val* v2 = op2->Eval(f);
	if ( ! v2 )
		{
		Unref(v1);
		return 0;
		}

	if ( is_vector(v1) )
		{
		VectorVal* vv = v1->AsVectorVal();
		if ( ! vv->Assign(vv->Size(), v2) )
			RuntimeError("type-checking failed in vector append");
		return v1;
		}

	Val* result = Fold(v1, v2);
	Unref(v1);
	Unref(v2);

	if ( result )
		{
		op1->Assign(f, result);
		return result->Ref();
		}
	else
		return 0;
	}

SubExpr::SubExpr(Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(EXPR_SUB, arg_op1, arg_op2)
	{
	if ( IsError() )
		return;

	const BroType* t1 = op1->Type();
	const BroType* t2 = op2->Type();

	TypeTag bt1 = t1->Tag();
	if ( IsVector(bt1) )
		bt1 = t1->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = t2->Tag();
	if ( IsVector(bt2) )
		bt2 = t2->AsVectorType()->YieldType()->Tag();

	BroType* base_result_type = 0;

	if ( bt1 == TYPE_TIME && bt2 == TYPE_INTERVAL )
		base_result_type = base_type(bt1);

	else if ( bt1 == TYPE_TIME && bt2 == TYPE_TIME )
		SetType(base_type(TYPE_INTERVAL));

	else if ( bt1 == TYPE_INTERVAL && bt2 == TYPE_INTERVAL )
		base_result_type = base_type(bt1);

	else if ( t1->IsSet() && t2->IsSet() )
		{
		if ( same_type(t1, t2) )
			SetType(op1->Type()->Ref());
		else
			ExprError("incompatible \"set\" operands");
		}

	else if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1) || is_vector(op2));

	else
		ExprError("requires arithmetic operands");

	if ( base_result_type )
		{
		if ( is_vector(op1) || is_vector(op2) )
			SetType(new VectorType(base_result_type));
		else
			SetType(base_result_type);
		}
	}

RemoveFromExpr::RemoveFromExpr(Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(EXPR_REMOVE_FROM, arg_op1->MakeLvalue(), arg_op2)
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();
	TypeTag bt2 = op2->Type()->Tag();

	if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1) || is_vector(op2));
	else if ( BothInterval(bt1, bt2) )
		SetType(base_type(bt1));
	else
		ExprError("requires two arithmetic operands");
	}

Val* RemoveFromExpr::Eval(Frame* f) const
	{
	Val* v1 = op1->Eval(f);
	if ( ! v1 )
		return 0;

	Val* v2 = op2->Eval(f);
	if ( ! v2 )
		{
		Unref(v1);
		return 0;
		}

	Val* result = Fold(v1, v2);

	Unref(v1);
	Unref(v2);

	if ( result )
		{
		op1->Assign(f, result);
		return result->Ref();
		}
	else
		return 0;
	}

TimesExpr::TimesExpr(Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(EXPR_TIMES, arg_op1, arg_op2)
	{
	if ( IsError() )
		return;

	Canonicize();

	TypeTag bt1 = op1->Type()->Tag();
	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = op2->Type()->Tag();
	if ( IsVector(bt2) )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	if ( bt1 == TYPE_INTERVAL || bt2 == TYPE_INTERVAL )
		{
		if ( IsArithmetic(bt1) || IsArithmetic(bt2) )
			PromoteType(TYPE_INTERVAL, is_vector(op1) || is_vector(op2) );
		else
			ExprError("multiplication with interval requires arithmetic operand");
		}
	else if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1) || is_vector(op2));
	else
		ExprError("requires arithmetic operands");
	}

void TimesExpr::Canonicize()
	{
	if ( expr_greater(op2, op1) || op2->Type()->Tag() == TYPE_INTERVAL ||
	     (op2->IsConst() && ! is_vector(op2->ExprVal()) && ! op1->IsConst()) )
		SwapOps();
	}

DivideExpr::DivideExpr(Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(EXPR_DIVIDE, arg_op1, arg_op2)
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();
	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = op2->Type()->Tag();
	if ( IsVector(bt2) )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	if ( bt1 == TYPE_INTERVAL || bt2 == TYPE_INTERVAL )
		{
		if ( IsArithmetic(bt1) || IsArithmetic(bt2) )
			PromoteType(TYPE_INTERVAL, is_vector(op1) || is_vector(op2));
		else if ( bt1 == TYPE_INTERVAL && bt2 == TYPE_INTERVAL )
			{
			if ( is_vector(op1) || is_vector(op2) )
				SetType(new VectorType(base_type(TYPE_DOUBLE)));
			else
				SetType(base_type(TYPE_DOUBLE));
			}
		else
			ExprError("division of interval requires arithmetic operand");
		}

	else if ( BothArithmetic(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1) || is_vector(op2));

	else if ( bt1 == TYPE_ADDR && ! is_vector(op2) &&
		  (bt2 == TYPE_COUNT || bt2 == TYPE_INT) )
		SetType(base_type(TYPE_SUBNET));

	else
		ExprError("requires arithmetic operands");
	}

Val* DivideExpr::AddrFold(Val* v1, Val* v2) const
	{
	uint32 mask;
	if ( v2->Type()->Tag() == TYPE_COUNT )
		mask = static_cast<uint32>(v2->InternalUnsigned());
	else
		mask = static_cast<uint32>(v2->InternalInt());

	auto& a = v1->AsAddr();

	if ( a.GetFamily() == IPv4 )
		{
		if ( mask > 32 )
			RuntimeError(fmt("bad IPv4 subnet prefix length: %" PRIu32, mask));
		}
	else
		{
		if ( mask > 128 )
			RuntimeError(fmt("bad IPv6 subnet prefix length: %" PRIu32, mask));
		}

	return new SubNetVal(a, mask);
	}

ModExpr::ModExpr(Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(EXPR_MOD, arg_op1, arg_op2)
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();
	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = op2->Type()->Tag();
	if ( IsVector(bt2) )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	if ( BothIntegral(bt1, bt2) )
		PromoteType(max_type(bt1, bt2), is_vector(op1) || is_vector(op2));
	else
		ExprError("requires integral operands");
	}

BoolExpr::BoolExpr(BroExprTag arg_tag, Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(arg_tag, arg_op1, arg_op2)
	{
	if ( IsError() )
		return;

	TypeTag bt1 = op1->Type()->Tag();
	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = op2->Type()->Tag();
	if ( IsVector(bt2) )
		bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

	if ( BothBool(bt1, bt2) )
		{
		if ( is_vector(op1) || is_vector(op2) )
			{
			if ( ! (is_vector(op1) && is_vector(op2)) )
				reporter->Warning("mixing vector and scalar operands is deprecated");
			SetType(new VectorType(base_type(TYPE_BOOL)));
			}
		else
			SetType(base_type(TYPE_BOOL));
		}
	else
		ExprError("requires boolean operands");
	}

Val* BoolExpr::DoSingleEval(Frame* f, Val* v1, Expr* op2) const
	{
	if ( ! v1 )
		return 0;

	if ( tag == EXPR_AND_AND )
		{
		if ( v1->IsZero() )
			return v1;
		else
			{
			Unref(v1);
			return op2->Eval(f);
			}
		}

	else
		{
		if ( v1->IsZero() )
			{
			Unref(v1);
			return op2->Eval(f);
			}
		else
			return v1;
		}
	}


Val* BoolExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return 0;

	Val* v1 = op1->Eval(f);
	if ( ! v1 )
		return 0;

	int is_vec1 = is_vector(op1);
	int is_vec2 = is_vector(op2);

	// Handle scalar op scalar
	if ( ! is_vec1 && ! is_vec2 )
		return DoSingleEval(f, v1, op2);

	// Handle scalar op vector  or  vector op scalar
	// We can't short-circuit everything since we need to eval
	// a vector in order to find out its length.
	if ( ! (is_vec1 && is_vec2) )
		{ // Only one is a vector.
		Val* scalar_v = 0;
		VectorVal* vector_v = 0;

		if ( is_vec1 )
			{
			scalar_v = op2->Eval(f);
			vector_v = v1->AsVectorVal();
			}
		else
			{
			scalar_v = v1;
			vector_v = op2->Eval(f)->AsVectorVal();
			}

		if ( ! scalar_v || ! vector_v )
			return 0;

		VectorVal* result = 0;

		// It's either an EXPR_AND_AND or an EXPR_OR_OR.
		bool is_and = (tag == EXPR_AND_AND);

		if ( scalar_v->IsZero() == is_and )
			{
			result = new VectorVal(Type()->AsVectorType());
			result->Resize(vector_v->Size());
			result->AssignRepeat(0, result->Size(),
						scalar_v);
			}
		else
			result = vector_v->Ref()->AsVectorVal();

		Unref(scalar_v);
		Unref(vector_v);

		return result;
		}

	// Only case remaining: both are vectors.
	Val* v2 = op2->Eval(f);
	if ( ! v2 )
		return 0;

	VectorVal* vec_v1 = v1->AsVectorVal();
	VectorVal* vec_v2 = v2->AsVectorVal();

	if ( vec_v1->Size() != vec_v2->Size() )
		{
		RuntimeError("vector operands have different sizes");
		return 0;
		}

	VectorVal* result = new VectorVal(Type()->AsVectorType());
	result->Resize(vec_v1->Size());

	for ( unsigned int i = 0; i < vec_v1->Size(); ++i )
		{
		Val* op1 = vec_v1->Lookup(i);
		Val* op2 = vec_v2->Lookup(i);
		if ( op1 && op2 )
			{
			bool local_result = (tag == EXPR_AND_AND) ?
				(! op1->IsZero() && ! op2->IsZero()) :
				(! op1->IsZero() || ! op2->IsZero());

			result->Assign(i, val_mgr->GetBool(local_result));
			}
		else
			result->Assign(i, 0);
		}

	Unref(v1);
	Unref(v2);

	return result;
	}

BitExpr::BitExpr(BroExprTag arg_tag, Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(arg_tag, arg_op1, arg_op2)
	{
	if ( IsError() )
		return;

	const BroType* t1 = op1->Type();
	const BroType* t2 = op2->Type();

	TypeTag bt1 = t1->Tag();
	if ( IsVector(bt1) )
		bt1 = t1->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = t2->Tag();
	if ( IsVector(bt2) )
		bt2 = t2->AsVectorType()->YieldType()->Tag();

	if ( (bt1 == TYPE_COUNT || bt1 == TYPE_COUNTER) &&
	     (bt2 == TYPE_COUNT || bt2 == TYPE_COUNTER) )
		{
		if ( bt1 == TYPE_COUNTER && bt2 == TYPE_COUNTER )
			ExprError("cannot apply a bitwise operator to two \"counter\" operands");
		else if ( is_vector(op1) || is_vector(op2) )
			SetType(new VectorType(base_type(TYPE_COUNT)));
		else
			SetType(base_type(TYPE_COUNT));
		}

	else if ( bt1 == TYPE_PATTERN )
		{
		if ( bt2 != TYPE_PATTERN )
			ExprError("cannot mix pattern and non-pattern operands");
		else if ( tag == EXPR_XOR )
			ExprError("'^' operator does not apply to patterns");
		else
			SetType(base_type(TYPE_PATTERN));
		}

	else if ( t1->IsSet() && t2->IsSet() )
		{
		if ( same_type(t1, t2) )
			SetType(op1->Type()->Ref());
		else
			ExprError("incompatible \"set\" operands");
		}

	else
		ExprError("requires \"count\" or compatible \"set\" operands");
	}

EqExpr::EqExpr(BroExprTag arg_tag, Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(arg_tag, arg_op1, arg_op2)
	{
	if ( IsError() )
		return;

	Canonicize();

	const BroType* t1 = op1->Type();
	const BroType* t2 = op2->Type();

	TypeTag bt1 = t1->Tag();
	if ( IsVector(bt1) )
		bt1 = t1->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = t2->Tag();
	if ( IsVector(bt2) )
		bt2 = t2->AsVectorType()->YieldType()->Tag();

	if ( is_vector(op1) || is_vector(op2) )
		SetType(new VectorType(base_type(TYPE_BOOL)));
	else
		SetType(base_type(TYPE_BOOL));

	if ( BothArithmetic(bt1, bt2) )
		PromoteOps(max_type(bt1, bt2));

	else if ( EitherArithmetic(bt1, bt2) &&
		// Allow comparisons with zero.
		  ((bt1 == TYPE_TIME && op2->IsZero()) ||
		   (bt2 == TYPE_TIME && op1->IsZero())) )
		PromoteOps(TYPE_TIME);

	else if ( bt1 == bt2 )
		{
		switch ( bt1 ) {
		case TYPE_BOOL:
		case TYPE_TIME:
		case TYPE_INTERVAL:
		case TYPE_STRING:
		case TYPE_PORT:
		case TYPE_ADDR:
		case TYPE_SUBNET:
		case TYPE_ERROR:
			break;

		case TYPE_ENUM:
			if ( ! same_type(t1, t2) )
				ExprError("illegal enum comparison");
			break;

		case TYPE_TABLE:
			if ( t1->IsSet() && t2->IsSet() )
				{
				if ( ! same_type(t1, t2) )
					ExprError("incompatible sets in comparison");
				break;
				}

			// FALL THROUGH

		default:
			ExprError("illegal comparison");
		}
		}

	else if ( bt1 == TYPE_PATTERN && bt2 == TYPE_STRING )
		;

	else
		ExprError("type clash in comparison");
	}

void EqExpr::Canonicize()
	{
	if ( op2->Type()->Tag() == TYPE_PATTERN )
		SwapOps();

	else if ( op1->Type()->Tag() == TYPE_PATTERN )
		;

	else if ( expr_greater(op2, op1) )
		SwapOps();
	}

Val* EqExpr::Fold(Val* v1, Val* v2) const
	{
	if ( op1->Type()->Tag() == TYPE_PATTERN )
		{
		RE_Matcher* re = v1->AsPattern();
		const BroString* s = v2->AsString();
		if ( tag == EXPR_EQ )
			return val_mgr->GetBool(re->MatchExactly(s));
		else
			return val_mgr->GetBool(! re->MatchExactly(s));
		}

	else
		return BinaryExpr::Fold(v1, v2);
	}

RelExpr::RelExpr(BroExprTag arg_tag, Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(arg_tag, arg_op1, arg_op2)
	{
	if ( IsError() )
		return;

	Canonicize();

	const BroType* t1 = op1->Type();
	const BroType* t2 = op2->Type();

	TypeTag bt1 = t1->Tag();
	if ( IsVector(bt1) )
		bt1 = t1->AsVectorType()->YieldType()->Tag();

	TypeTag bt2 = t2->Tag();
	if ( IsVector(bt2) )
		bt2 = t2->AsVectorType()->YieldType()->Tag();

	if ( is_vector(op1) || is_vector(op2) )
		SetType(new VectorType(base_type(TYPE_BOOL)));
	else
		SetType(base_type(TYPE_BOOL));

	if ( BothArithmetic(bt1, bt2) )
		PromoteOps(max_type(bt1, bt2));

	else if ( t1->IsSet() && t2->IsSet() )
		{
		if ( ! same_type(t1, t2) )
			ExprError("incompatible sets in comparison");
		}

	else if ( bt1 != bt2 )
		ExprError("operands must be of the same type");

	else if ( bt1 != TYPE_TIME && bt1 != TYPE_INTERVAL &&
		  bt1 != TYPE_PORT && bt1 != TYPE_ADDR &&
		  bt1 != TYPE_STRING )
		ExprError("illegal comparison");
	}

void RelExpr::Canonicize()
	{
	if ( tag == EXPR_GT )
		{
		SwapOps();
		tag = EXPR_LT;
		}

	else if ( tag == EXPR_GE )
		{
		SwapOps();
		tag = EXPR_LE;
		}
	}

CondExpr::CondExpr(Expr* arg_op1, Expr* arg_op2, Expr* arg_op3)
: Expr(EXPR_COND)
	{
	op1 = arg_op1;
	op2 = arg_op2;
	op3 = arg_op3;

	TypeTag bt1 = op1->Type()->Tag();
	if ( IsVector(bt1) )
		bt1 = op1->Type()->AsVectorType()->YieldType()->Tag();

	if ( op1->IsError() || op2->IsError() || op3->IsError() )
		SetError();

	else if ( bt1 != TYPE_BOOL )
		ExprError("requires boolean conditional");

	else
		{
		TypeTag bt2 = op2->Type()->Tag();
		if ( is_vector(op2) )
			bt2 = op2->Type()->AsVectorType()->YieldType()->Tag();

		TypeTag bt3 = op3->Type()->Tag();
		if ( IsVector(bt3) )
			bt3 = op3->Type()->AsVectorType()->YieldType()->Tag();

		if ( is_vector(op1) && ! (is_vector(op2) && is_vector(op3)) )
			{
			ExprError("vector conditional requires vector alternatives");
			return;
			}

		if ( BothArithmetic(bt2, bt3) )
			{
			TypeTag t = max_type(bt2, bt3);
			if ( bt2 != t )
				op2 = new ArithCoerceExpr(op2, t);
			if ( bt3 != t )
				op3 = new ArithCoerceExpr(op3, t);

			if ( is_vector(op2) )
				SetType(new VectorType(base_type(t)));
			else
				SetType(base_type(t));
			}

		else if ( bt2 != bt3 )
			ExprError("operands must be of the same type");

		else
			{
			if ( IsRecord(bt2) && IsRecord(bt3) &&
			     ! same_type(op2->Type(), op3->Type()) )
				ExprError("operands must be of the same type");
			else
				SetType(op2->Type()->Ref());
			}
		}
	}

CondExpr::~CondExpr()
	{
	Unref(op1);
	Unref(op2);
	Unref(op3);
	}

Val* CondExpr::Eval(Frame* f) const
	{
	if ( ! is_vector(op1) )
		{ // scalar is easy
		Val* v = op1->Eval(f);
		int false_eval = v->IsZero();
		Unref(v);

		return (false_eval ? op3 : op2)->Eval(f);
		}

	// Vector case: no mixed scalar/vector cases allowed
	Val* v1 = op1->Eval(f);
	if ( ! v1 )
		return 0;

	Val* v2 = op2->Eval(f);
	if ( ! v2 )
		return 0;

	Val* v3 = op3->Eval(f);
	if ( ! v3 )
		return 0;

	VectorVal* cond = v1->AsVectorVal();
	VectorVal* a = v2->AsVectorVal();
	VectorVal* b = v3->AsVectorVal();

	if ( cond->Size() != a->Size() || a->Size() != b->Size() )
		{
		RuntimeError("vectors in conditional expression have different sizes");
		return 0;
		}

	VectorVal* result = new VectorVal(Type()->AsVectorType());
	result->Resize(cond->Size());

	for ( unsigned int i = 0; i < cond->Size(); ++i )
		{
		Val* local_cond = cond->Lookup(i);
		if ( local_cond )
			result->Assign(i,
				       local_cond->IsZero() ?
					       b->Lookup(i) : a->Lookup(i));
		else
			result->Assign(i, 0);
		}

	return result;
	}

int CondExpr::IsPure() const
	{
	return op1->IsPure() && op2->IsPure() && op3->IsPure();
	}

TraversalCode CondExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op1->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op2->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op3->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void CondExpr::ExprDescribe(ODesc* d) const
	{
	op1->Describe(d);
	d->AddSP(" ?");
	op2->Describe(d);
	d->AddSP(" :");
	op3->Describe(d);
	}

RefExpr::RefExpr(Expr* arg_op) : UnaryExpr(EXPR_REF, arg_op)
	{
	if ( IsError() )
		return;

	if ( ! ::is_assignable(op->Type()) )
		ExprError("illegal assignment target");
	else
		SetType(op->Type()->Ref());
	}

Expr* RefExpr::MakeLvalue()
	{
	return this;
	}

void RefExpr::Assign(Frame* f, Val* v)
	{
	op->Assign(f, v);
	}

AssignExpr::AssignExpr(Expr* arg_op1, Expr* arg_op2, int arg_is_init,
		       Val* arg_val, attr_list* arg_attrs)
: BinaryExpr(EXPR_ASSIGN,
		arg_is_init ? arg_op1 : arg_op1->MakeLvalue(), arg_op2)
	{
	val = 0;
	is_init = arg_is_init;

	if ( IsError() )
		return;

	SetType(arg_val ? arg_val->Type()->Ref() : op1->Type()->Ref());

	if ( is_init )
		{
		SetLocationInfo(arg_op1->GetLocationInfo(),
				arg_op2->GetLocationInfo());
		return;
		}

	// We discard the status from TypeCheck since it has already
	// generated error messages.
	(void) TypeCheck(arg_attrs);

	val = arg_val ? arg_val->Ref() : 0;

	SetLocationInfo(arg_op1->GetLocationInfo(), arg_op2->GetLocationInfo());
	}

bool AssignExpr::TypeCheck(attr_list* attrs)
	{
	TypeTag bt1 = op1->Type()->Tag();
	TypeTag bt2 = op2->Type()->Tag();

	if ( bt1 == TYPE_LIST && bt2 == TYPE_ANY )
		// This is ok because we cannot explicitly declare lists on
		// the script level.
		return true;

	if ( ((bt1 == TYPE_ENUM) ^ (bt2 == TYPE_ENUM)) )
		{
		ExprError("can't convert to/from enumerated type");
		return false;
		}

	if ( IsArithmetic(bt1) )
		return TypeCheckArithmetics(bt1, bt2);

	if ( bt1 == TYPE_TIME && IsArithmetic(bt2) && op2->IsZero() )
		{ // Allow assignments to zero as a special case.
		op2 = new ArithCoerceExpr(op2, bt1);
		return true;
		}

	if ( bt1 == TYPE_TABLE && bt2 == bt1 &&
	     op2->Type()->AsTableType()->IsUnspecifiedTable() )
		{
		op2 = new TableCoerceExpr(op2, op1->Type()->AsTableType());
		return true;
		}

	if ( bt1 == TYPE_TABLE && op2->Tag() == EXPR_LIST )
		{
		attr_list* attr_copy = 0;
		if ( attrs )
			{
			attr_copy = new attr_list(attrs->length());
			std::copy(attrs->begin(), attrs->end(), std::back_inserter(*attr_copy));
			}

		bool empty_list_assignment = (op2->AsListExpr()->Exprs().length() == 0);

		if ( op1->Type()->IsSet() )
			op2 = new SetConstructorExpr(op2->AsListExpr(), attr_copy);
		else
			op2 = new TableConstructorExpr(op2->AsListExpr(), attr_copy);

		if ( ! empty_list_assignment && ! same_type(op1->Type(), op2->Type()) )
			{
			if ( op1->Type()->IsSet() )
				ExprError("set type mismatch in assignment");
			else
				ExprError("table type mismatch in assignment");

			return false;
			}

		return true;
		}

	if ( bt1 == TYPE_VECTOR )
		{
		if ( bt2 == bt1 && op2->Type()->AsVectorType()->IsUnspecifiedVector() )
			{
			op2 = new VectorCoerceExpr(op2, op1->Type()->AsVectorType());
			return true;
			}

		if ( op2->Tag() == EXPR_LIST )
			{
			op2 = new VectorConstructorExpr(op2->AsListExpr(), op1->Type());
			return true;
			}
		}

	if ( op1->Type()->Tag() == TYPE_RECORD &&
	     op2->Type()->Tag() == TYPE_RECORD )
		{
		if ( same_type(op1->Type(), op2->Type()) )
			{
			RecordType* rt1 = op1->Type()->AsRecordType();
			RecordType* rt2 = op2->Type()->AsRecordType();

			// Make sure the attributes match as well.
			for ( int i = 0; i < rt1->NumFields(); ++i )
				{
				const TypeDecl* td1 = rt1->FieldDecl(i);
				const TypeDecl* td2 = rt2->FieldDecl(i);

				if ( same_attrs(td1->attrs, td2->attrs) )
					// Everything matches.
					return true;
				}
			}

		// Need to coerce.
		op2 = new RecordCoerceExpr(op2, op1->Type()->AsRecordType());
		return true;
		}

	if ( ! same_type(op1->Type(), op2->Type()) )
		{
		if ( bt1 == TYPE_TABLE && bt2 == TYPE_TABLE )
			{
			if ( op2->Tag() == EXPR_SET_CONSTRUCTOR )
				{
				// Some elements in constructor list must not match, see if
				// we can create a new constructor now that the expected type
				// of LHS is known and let it do coercions where possible.
				SetConstructorExpr* sce = dynamic_cast<SetConstructorExpr*>(op2);
				ListExpr* ctor_list = dynamic_cast<ListExpr*>(sce->Op());
				attr_list* attr_copy = 0;

				if ( sce->Attrs() )
					{
					attr_list* a = sce->Attrs()->Attrs();
					attrs = new attr_list(a->length());
					std::copy(a->begin(), a->end(), std::back_inserter(*attrs));
					}

				int errors_before = reporter->Errors();
				op2 = new SetConstructorExpr(ctor_list, attr_copy, op1->Type());
				int errors_after = reporter->Errors();

				if ( errors_after > errors_before )
					{
					ExprError("type clash in assignment");
					return false;
					}

				return true;
				}
			}

		ExprError("type clash in assignment");
		return false;
		}

	return true;
	}

bool AssignExpr::TypeCheckArithmetics(TypeTag bt1, TypeTag bt2)
	{
	if ( ! IsArithmetic(bt2) )
		{
		char err[512];
		snprintf(err, sizeof(err),
			"assignment of non-arithmetic value to arithmetic (%s/%s)",
			 type_name(bt1), type_name(bt2));
		ExprError(err);
		return false;
		}

	if ( bt1 == TYPE_DOUBLE )
		{
		PromoteOps(TYPE_DOUBLE);
		return true;
		}

	if ( bt2 == TYPE_DOUBLE )
		{
		Warn("dangerous assignment of double to integral");
		op2 = new ArithCoerceExpr(op2, bt1);
		bt2 = op2->Type()->Tag();
		}

	if ( bt1 == TYPE_INT )
		PromoteOps(TYPE_INT);
	else
		{
		if ( bt2 == TYPE_INT )
			{
			Warn("dangerous assignment of integer to count");
			op2 = new ArithCoerceExpr(op2, bt1);
			bt2 = op2->Type()->Tag();
			}

		// Assignment of count to counter or vice
		// versa is allowed, and requires no
		// coercion.
		}

	return true;
	}


Val* AssignExpr::Eval(Frame* f) const
	{
	if ( is_init )
		{
		RuntimeError("illegal assignment in initialization");
		return 0;
		}

	Val* v = op2->Eval(f);

	if ( v )
		{
		op1->Assign(f, v);
		return val ? val->Ref() : v->Ref();
		}
	else
		return 0;
	}

BroType* AssignExpr::InitType() const
	{
	if ( op1->Tag() != EXPR_LIST )
		{
		Error("bad initializer");
		return 0;
		}

	BroType* tl = op1->Type();
	if ( tl->Tag() != TYPE_LIST )
		Internal("inconsistent list expr in AssignExpr::InitType");

	return new TableType(tl->Ref()->AsTypeList(), op2->Type()->Ref());
	}

void AssignExpr::EvalIntoAggregate(const BroType* t, Val* aggr, Frame* f) const
	{
	if ( IsError() )
		return;

	TypeDecl td(0, 0);
	if ( IsRecordElement(&td) )
		{
		if ( t->Tag() != TYPE_RECORD )
			{
			RuntimeError("not a record initializer");
			return;
			}

		const RecordType* rt = t->AsRecordType();
		int field = rt->FieldOffset(td.id);

		if ( field < 0 )
			{
			RuntimeError("no such field");
			return;
			}

		RecordVal* aggr_r = aggr->AsRecordVal();

		Val* v = op2->Eval(f);
		if ( v )
			aggr_r->Assign(field, v);

		return;
		}

	if ( op1->Tag() != EXPR_LIST )
		RuntimeError("bad table insertion");

	TableVal* tv = aggr->AsTableVal();

	Val* index = op1->Eval(f);
	Val* v = check_and_promote(op2->Eval(f), t->YieldType(), 1);
	if ( ! index || ! v )
		return;

	if ( ! tv->Assign(index, v) )
		RuntimeError("type clash in table assignment");

	Unref(index);
	}

Val* AssignExpr::InitVal(const BroType* t, Val* aggr) const
	{
	if ( ! aggr )
		{
		Error("assignment in initialization");
		return 0;
		}

	if ( IsError() )
		return 0;

	TypeDecl td(0, 0);
	if ( IsRecordElement(&td) )
		{
		if ( t->Tag() != TYPE_RECORD )
			{
			Error("not a record initializer", t);
			return 0;
			}
		const RecordType* rt = t->AsRecordType();
		int field = rt->FieldOffset(td.id);

		if ( field < 0 )
			{
			Error("no such field");
			return 0;
			}

		if ( aggr->Type()->Tag() != TYPE_RECORD )
			Internal("bad aggregate in AssignExpr::InitVal");
		RecordVal* aggr_r = aggr->AsRecordVal();

		Val* v = op2->InitVal(rt->FieldType(td.id), 0);
		if ( ! v )
			return 0;

		aggr_r->Assign(field, v);
		return v;
		}

	else if ( op1->Tag() == EXPR_LIST )
		{
		if ( t->Tag() != TYPE_TABLE )
			{
			Error("not a table initialization", t);
			return 0;
			}

		if ( aggr->Type()->Tag() != TYPE_TABLE )
			Internal("bad aggregate in AssignExpr::InitVal");

		TableVal* tv = aggr->AsTableVal();
		const TableType* tt = tv->Type()->AsTableType();
		const BroType* yt = tv->Type()->YieldType();
		Val* index = op1->InitVal(tt->Indices(), 0);
		Val* v = op2->InitVal(yt, 0);
		if ( ! index || ! v )
			return 0;

		if ( ! tv->ExpandAndInit(index, v) )
			{
			Unref(index);
			Unref(tv);
			return 0;
			}

		Unref(index);
		return tv;
		}

	else
		{
		Error("illegal initializer");
		return 0;
		}
	}

int AssignExpr::IsRecordElement(TypeDecl* td) const
	{
	if ( op1->Tag() == EXPR_NAME )
		{
		if ( td )
			{
			const NameExpr* n = (const NameExpr*) op1;
			td->type = op2->Type()->Ref();
			td->id = copy_string(n->Id()->Name());
			}

		return 1;
		}
	else
		return 0;
	}

int AssignExpr::IsPure() const
	{
	return 0;
	}

IndexSliceAssignExpr::IndexSliceAssignExpr(Expr* op1, Expr* op2, int is_init)
	: AssignExpr(op1, op2, is_init)
	{
	}

Val* IndexSliceAssignExpr::Eval(Frame* f) const
	{
	if ( is_init )
		{
		RuntimeError("illegal assignment in initialization");
		return 0;
		}

	Val* v = op2->Eval(f);

	if ( v )
		{
		op1->Assign(f, v);
		Unref(v);
		}

	return 0;
	}

IndexExpr::IndexExpr(Expr* arg_op1, ListExpr* arg_op2, bool arg_is_slice)
: BinaryExpr(EXPR_INDEX, arg_op1, arg_op2), is_slice(arg_is_slice)
	{
	if ( IsError() )
		return;

	if ( is_slice )
		{
		if ( ! IsString(op1->Type()->Tag()) && ! IsVector(op1->Type()->Tag()) )
			ExprError("slice notation indexing only supported for strings and vectors currently");
		}

	else if ( IsString(op1->Type()->Tag()) )
		{
		if ( arg_op2->Exprs().length() != 1 )
			ExprError("invalid string index expression");
		}

	if ( IsError() )
		return;

	int match_type = op1->Type()->MatchesIndex(arg_op2);
	if ( match_type == DOES_NOT_MATCH_INDEX )
		{
		std::string error_msg =
		    fmt("expression with type '%s' is not a type that can be indexed",
		        type_name(op1->Type()->Tag()));
		SetError(error_msg.data());
		}

	else if ( ! op1->Type()->YieldType() )
		{
		if ( IsString(op1->Type()->Tag()) && match_type == MATCHES_INDEX_SCALAR )
			SetType(base_type(TYPE_STRING));
		else
		// It's a set - so indexing it yields void.  We don't
		// directly generate an error message, though, since this
		// expression might be part of an add/delete statement,
		// rather than yielding a value.
			SetType(base_type(TYPE_VOID));
		}

	else if ( match_type == MATCHES_INDEX_SCALAR )
		SetType(op1->Type()->YieldType()->Ref());

	else if ( match_type == MATCHES_INDEX_VECTOR )
		SetType(new VectorType(op1->Type()->YieldType()->Ref()));

	else
		ExprError("Unknown MatchesIndex() return value");

	}

int IndexExpr::CanAdd() const
	{
	if ( IsError() )
		return 1;	// avoid cascading the error report

	// "add" only allowed if our type is "set".
	return op1->Type()->IsSet();
	}

int IndexExpr::CanDel() const
	{
	if ( IsError() )
		return 1;	// avoid cascading the error report

	return op1->Type()->Tag() == TYPE_TABLE;
	}

void IndexExpr::Add(Frame* f)
	{
	if ( IsError() )
		return;

	Val* v1 = op1->Eval(f);
	if ( ! v1 )
		return;

	Val* v2 = op2->Eval(f);
	if ( ! v2 )
		{
		Unref(v1);
		return;
		}

	v1->AsTableVal()->Assign(v2, 0);

	Unref(v1);
	Unref(v2);
	}

void IndexExpr::Delete(Frame* f)
	{
	if ( IsError() )
		return;

	Val* v1 = op1->Eval(f);
	if ( ! v1 )
		return;

	Val* v2 = op2->Eval(f);
	if ( ! v2 )
		{
		Unref(v1);
		return;
		}

	Unref(v1->AsTableVal()->Delete(v2));

	Unref(v1);
	Unref(v2);
	}

Expr* IndexExpr::MakeLvalue()
	{
	if ( IsString(op1->Type()->Tag()) )
		ExprError("cannot assign to string index expression");

	return new RefExpr(this);
	}

Val* IndexExpr::Eval(Frame* f) const
	{
	Val* v1 = op1->Eval(f);
	if ( ! v1 )
		return 0;

	Val* v2 = op2->Eval(f);
	if ( ! v2 )
		{
		Unref(v1);
		return 0;
		}

	Val* result;

	Val* indv = v2->AsListVal()->Index(0);
	if ( is_vector(indv) )
		{
		VectorVal* v_v1 = v1->AsVectorVal();
		VectorVal* v_v2 = indv->AsVectorVal();
		VectorVal* v_result = new VectorVal(Type()->AsVectorType());
		result = v_result;

		// Booleans select each element (or not).
		if ( IsBool(v_v2->Type()->YieldType()->Tag()) )
			{
			if ( v_v1->Size() != v_v2->Size() )
				{
				RuntimeError("size mismatch, boolean index and vector");
				Unref(v_result);
				return 0;
				}

			for ( unsigned int i = 0; i < v_v2->Size(); ++i )
				{
				if ( v_v2->Lookup(i)->AsBool() )
					v_result->Assign(v_result->Size() + 1, v_v1->Lookup(i));
				}
			}
		else
			{ // The elements are indices.
			// ### Should handle negative indices here like
			// S does, i.e., by excluding those elements.
			// Probably only do this if *all* are negative.
			v_result->Resize(v_v2->Size());
			for ( unsigned int i = 0; i < v_v2->Size(); ++i )
				v_result->Assign(i, v_v1->Lookup(v_v2->Lookup(i)->CoerceToInt()));
			}
		}
	else
		result = Fold(v1, v2);

	Unref(v1);
	Unref(v2);
	return result;
	}

static int get_slice_index(int idx, int len)
	{
	if ( abs(idx) > len )
		idx = idx > 0 ? len : 0; // Clamp maximum positive/negative indices.
	else if ( idx < 0 )
		idx += len;  // Map to a positive index.

	return idx;
	}

Val* IndexExpr::Fold(Val* v1, Val* v2) const
	{
	if ( IsError() )
		return 0;

	Val* v = 0;

	switch ( v1->Type()->Tag() ) {
	case TYPE_VECTOR:
		{
		VectorVal* vect = v1->AsVectorVal();
		const ListVal* lv = v2->AsListVal();

		if ( lv->Length() == 1 )
			v = vect->Lookup(v2);
		else
			{
			int len = vect->Size();
			VectorVal* result = new VectorVal(vect->Type()->AsVectorType());

			bro_int_t first = get_slice_index(lv->Index(0)->CoerceToInt(), len);
			bro_int_t last = get_slice_index(lv->Index(1)->CoerceToInt(), len);
			int sub_length = last - first;

			if ( sub_length >= 0 )
				{
				result->Resize(sub_length);

				for ( int idx = first; idx < last; idx++ )
					result->Assign(idx - first, vect->Lookup(idx)->Ref());
				}

			return result;
			}
		}
		break;

	case TYPE_TABLE:
		v = v1->AsTableVal()->Lookup(v2); // Then, we jump into the TableVal here.
		break;

	case TYPE_STRING:
		{
		const ListVal* lv = v2->AsListVal();
		const BroString* s = v1->AsString();
		int len = s->Len();
		BroString* substring = 0;

		if ( lv->Length() == 1 )
			{
			bro_int_t idx = lv->Index(0)->AsInt();

			if ( idx < 0 )
				idx += len;

			// Out-of-range index will return null pointer.
			substring = s->GetSubstring(idx, 1);
			}
		else
			{
			bro_int_t first = get_slice_index(lv->Index(0)->AsInt(), len);
			bro_int_t last = get_slice_index(lv->Index(1)->AsInt(), len);
			int substring_len = last - first;

			if ( substring_len < 0 )
				substring = 0;
			else
				substring = s->GetSubstring(first, substring_len);
			}

		return new StringVal(substring ? substring : new BroString(""));
		}

	default:
		RuntimeError("type cannot be indexed");
		break;
	}

	if ( v )
		return v->Ref();

	RuntimeError("no such index");
	return 0;
	}

void IndexExpr::Assign(Frame* f, Val* v)
	{
	if ( IsError() )
		return;

	Val* v1 = op1->Eval(f);
	if ( ! v1 )
		return;

	Val* v2 = op2->Eval(f);

	if ( ! v1 || ! v2 )
		{
		Unref(v1);
		Unref(v2);
		return;
		}

	switch ( v1->Type()->Tag() ) {
	case TYPE_VECTOR:
		{
		const ListVal* lv = v2->AsListVal();
		VectorVal* v1_vect = v1->AsVectorVal();

		if ( lv->Length() > 1 )
			{
			auto len = v1_vect->Size();
			bro_int_t first = get_slice_index(lv->Index(0)->CoerceToInt(), len);
			bro_int_t last = get_slice_index(lv->Index(1)->CoerceToInt(), len);

			// Remove the elements from the vector within the slice
			for ( auto idx = first; idx < last; idx++ )
				v1_vect->Remove(first);

			// Insert the new elements starting at the first position
			VectorVal* v_vect = v->AsVectorVal();

			for ( auto idx = 0u; idx < v_vect->Size(); idx++, first++ )
				v1_vect->Insert(first, v_vect->Lookup(idx)->Ref());
			}
		else if ( ! v1_vect->Assign(v2, v) )
			{
			if ( v )
				{
				ODesc d;
				v->Describe(&d);
				auto vt = v->Type();
				auto vtt = vt->Tag();
				std::string tn = vtt == TYPE_RECORD ? vt->GetName() : type_name(vtt);
				RuntimeErrorWithCallStack(fmt(
				  "vector index assignment failed for invalid type '%s', value: %s",
				  tn.data(), d.Description()));
				}
			else
				RuntimeErrorWithCallStack("assignment failed with null value");
			}
		break;
		}

	case TYPE_TABLE:
		if ( ! v1->AsTableVal()->Assign(v2, v) )
			{
			if ( v )
				{
				ODesc d;
				v->Describe(&d);
				auto vt = v->Type();
				auto vtt = vt->Tag();
				std::string tn = vtt == TYPE_RECORD ? vt->GetName() : type_name(vtt);
				RuntimeErrorWithCallStack(fmt(
				  "table index assignment failed for invalid type '%s', value: %s",
				  tn.data(), d.Description()));
				}
			else
				RuntimeErrorWithCallStack("assignment failed with null value");
			}
		break;

	case TYPE_STRING:
		RuntimeErrorWithCallStack("assignment via string index accessor not allowed");
		break;

	default:
		RuntimeErrorWithCallStack("bad index expression type in assignment");
		break;
	}

	Unref(v1);
	Unref(v2);
	}

void IndexExpr::ExprDescribe(ODesc* d) const
	{
	op1->Describe(d);
	if ( d->IsReadable() )
		d->Add("[");

	op2->Describe(d);
	if ( d->IsReadable() )
		d->Add("]");
	}

TraversalCode IndexExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op1->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = op2->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

FieldExpr::FieldExpr(Expr* arg_op, const char* arg_field_name)
: UnaryExpr(EXPR_FIELD, arg_op)
	{
	field_name = copy_string(arg_field_name);
	td = 0;
	field = 0;

	if ( IsError() )
		return;

	if ( ! IsRecord(op->Type()->Tag()) )
		ExprError("not a record");
	else
		{
		RecordType* rt = op->Type()->AsRecordType();
		field = rt->FieldOffset(field_name);

		if ( field < 0 )
			ExprError("no such field in record");
		else
			{
			SetType(rt->FieldType(field)->Ref());
			td = rt->FieldDecl(field);

			if ( rt->IsFieldDeprecated(field) )
				reporter->Warning("%s", rt->GetFieldDeprecationWarning(field, false).c_str());
			}
		}
	}

FieldExpr::~FieldExpr()
	{
	delete [] field_name;
	}

Expr* FieldExpr::MakeLvalue()
	{
	return new RefExpr(this);
	}

int FieldExpr::CanDel() const
	{
	return td->FindAttr(ATTR_DEFAULT) || td->FindAttr(ATTR_OPTIONAL);
	}

void FieldExpr::Assign(Frame* f, Val* v)
	{
	if ( IsError() )
		return;

	Val* op_v = op->Eval(f);
	if ( op_v )
		{
		RecordVal* r = op_v->AsRecordVal();
		r->Assign(field, v);
		Unref(r);
		}
	}

void FieldExpr::Delete(Frame* f)
	{
	Assign(f, 0);
	}

Val* FieldExpr::Fold(Val* v) const
	{
	Val* result = v->AsRecordVal()->Lookup(field);
	if ( result )
		return result->Ref();

	// Check for &default.
	const Attr* def_attr = td ? td->FindAttr(ATTR_DEFAULT) : 0;
	if ( def_attr )
		return def_attr->AttrExpr()->Eval(0);
	else
		{
		RuntimeError("field value missing");
		assert(false);
		return 0; // Will never get here, but compiler can't tell.
		}
	}

void FieldExpr::ExprDescribe(ODesc* d) const
	{
	op->Describe(d);
	if ( d->IsReadable() )
		d->Add("$");

	if ( IsError() )
		d->Add("<error>");
	else if ( d->IsReadable() )
		d->Add(field_name);
	else
		d->Add(field);
	}

HasFieldExpr::HasFieldExpr(Expr* arg_op, const char* arg_field_name)
: UnaryExpr(EXPR_HAS_FIELD, arg_op)
	{
	field_name = arg_field_name;
	field = 0;

	if ( IsError() )
		return;

	if ( ! IsRecord(op->Type()->Tag()) )
		ExprError("not a record");
	else
		{
		RecordType* rt = op->Type()->AsRecordType();
		field = rt->FieldOffset(field_name);

		if ( field < 0 )
			ExprError("no such field in record");
		else if ( rt->IsFieldDeprecated(field) )
			reporter->Warning("%s", rt->GetFieldDeprecationWarning(field, true).c_str());

		SetType(base_type(TYPE_BOOL));
		}
	}

HasFieldExpr::~HasFieldExpr()
	{
	delete field_name;
	}

Val* HasFieldExpr::Fold(Val* v) const
	{
	RecordVal* rec_to_look_at;

	rec_to_look_at = v->AsRecordVal();

	if ( ! rec_to_look_at )
		return val_mgr->GetBool(0);

	RecordVal* r = rec_to_look_at->Ref()->AsRecordVal();
	Val* ret = val_mgr->GetBool(r->Lookup(field) != 0);
	Unref(r);

	return ret;
	}

void HasFieldExpr::ExprDescribe(ODesc* d) const
	{
	op->Describe(d);

	if ( d->IsReadable() )
		d->Add("?$");

	if ( IsError() )
		d->Add("<error>");
	else if ( d->IsReadable() )
		d->Add(field_name);
	else
		d->Add(field);
	}

RecordConstructorExpr::RecordConstructorExpr(ListExpr* constructor_list)
: UnaryExpr(EXPR_RECORD_CONSTRUCTOR, constructor_list)
	{
	if ( IsError() )
		return;

	// Spin through the list, which should be comprised only of
	// record-field-assign expressions, and build up a
	// record type to associate with this constructor.
	const expr_list& exprs = constructor_list->Exprs();
	type_decl_list* record_types = new type_decl_list(exprs.length());

	for ( const auto& e : exprs )
		{
		if ( e->Tag() != EXPR_FIELD_ASSIGN )
			{
			Error("bad type in record constructor", e);
			SetError();
			continue;
			}

		FieldAssignExpr* field = (FieldAssignExpr*) e;
		BroType* field_type = field->Type()->Ref();
		char* field_name = copy_string(field->FieldName());
		record_types->push_back(new TypeDecl(field_type, field_name));
		}

	SetType(new RecordType(record_types));
	}

RecordConstructorExpr::~RecordConstructorExpr()
	{
	}

Val* RecordConstructorExpr::InitVal(const BroType* t, Val* aggr) const
	{
	Val* v = Eval(0);

	if ( v )
		{
		RecordVal* rv = v->AsRecordVal();
		RecordVal* ar = rv->CoerceTo(t->AsRecordType(), aggr);

		if ( ar )
			{
			Unref(rv);
			return ar;
			}
		}

	Error("bad record initializer");
	return 0;
	}

Val* RecordConstructorExpr::Fold(Val* v) const
	{
	ListVal* lv = v->AsListVal();
	RecordType* rt = type->AsRecordType();

	if ( lv->Length() != rt->NumFields() )
		RuntimeErrorWithCallStack("inconsistency evaluating record constructor");

	RecordVal* rv = new RecordVal(rt);

	for ( int i = 0; i < lv->Length(); ++i )
		rv->Assign(i, lv->Index(i)->Ref());

	return rv;
	}

void RecordConstructorExpr::ExprDescribe(ODesc* d) const
	{
	d->Add("[");
	op->Describe(d);
	d->Add("]");
	}

TableConstructorExpr::TableConstructorExpr(ListExpr* constructor_list,
					   attr_list* arg_attrs, BroType* arg_type)
: UnaryExpr(EXPR_TABLE_CONSTRUCTOR, constructor_list)
	{
	attrs = 0;

	if ( IsError() )
		return;

	if ( arg_type )
		{
		if ( ! arg_type->IsTable() )
			{
			Error("bad table constructor type", arg_type);
			SetError();
			return;
			}

		SetType(arg_type->Ref());
		}
	else
		{
		if ( constructor_list->Exprs().length() == 0 )
			SetType(new TableType(new TypeList(base_type(TYPE_ANY)), 0));
		else
			{
			SetType(init_type(constructor_list));

			if ( ! type )
				SetError();

			else if ( type->Tag() != TYPE_TABLE ||
				  type->AsTableType()->IsSet() )
				SetError("values in table(...) constructor do not specify a table");
			}
		}

	attrs = arg_attrs ? new Attributes(arg_attrs, type, false, false) : 0;

	type_list* indices = type->AsTableType()->Indices()->Types();
	const expr_list& cle = constructor_list->Exprs();

	// check and promote all index expressions in ctor list
	for ( const auto& expr : cle )
		{
		if ( expr->Tag() != EXPR_ASSIGN )
			continue;

		Expr* idx_expr = expr->AsAssignExpr()->Op1();

		if ( idx_expr->Tag() != EXPR_LIST )
			continue;

		expr_list& idx_exprs = idx_expr->AsListExpr()->Exprs();

		if ( idx_exprs.length() != indices->length() )
			continue;

		loop_over_list(idx_exprs, j)
			{
			Expr* idx = idx_exprs[j];

			if ( check_and_promote_expr(idx, (*indices)[j]) )
				{
				if ( idx != idx_exprs[j] )
					idx_exprs.replace(j, idx);
				continue;
				}

			ExprError("inconsistent types in table constructor");
			}
		}
	}

Val* TableConstructorExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return 0;

	Val* aggr = new TableVal(Type()->AsTableType(), attrs);
	const expr_list& exprs = op->AsListExpr()->Exprs();

	for ( const auto& expr : exprs )
		expr->EvalIntoAggregate(type, aggr, f);

	aggr->AsTableVal()->InitDefaultFunc(f);

	return aggr;
	}

Val* TableConstructorExpr::InitVal(const BroType* t, Val* aggr) const
	{
	if ( IsError() )
		return 0;

	TableType* tt = Type()->AsTableType();
	TableVal* tval = aggr ? aggr->AsTableVal() : new TableVal(tt, attrs);
	const expr_list& exprs = op->AsListExpr()->Exprs();

	for ( const auto& expr : exprs )
		expr->EvalIntoAggregate(t, tval, 0);

	return tval;
	}

void TableConstructorExpr::ExprDescribe(ODesc* d) const
	{
	d->Add("table(");
	op->Describe(d);
	d->Add(")");
	}

SetConstructorExpr::SetConstructorExpr(ListExpr* constructor_list,
				       attr_list* arg_attrs, BroType* arg_type)
: UnaryExpr(EXPR_SET_CONSTRUCTOR, constructor_list)
	{
	attrs = 0;

	if ( IsError() )
		return;

	if ( arg_type )
		{
		if ( ! arg_type->IsSet() )
			{
			Error("bad set constructor type", arg_type);
			SetError();
			return;
			}

		SetType(arg_type->Ref());
		}
	else
		{
		if ( constructor_list->Exprs().length() == 0 )
			SetType(new ::SetType(new TypeList(base_type(TYPE_ANY)), 0));
		else
			SetType(init_type(constructor_list));
		}

	if ( ! type )
		SetError();

	else if ( type->Tag() != TYPE_TABLE || ! type->AsTableType()->IsSet() )
		SetError("values in set(...) constructor do not specify a set");

	attrs = arg_attrs ? new Attributes(arg_attrs, type, false, false) : 0;

	type_list* indices = type->AsTableType()->Indices()->Types();
	expr_list& cle = constructor_list->Exprs();

	if ( indices->length() == 1 )
		{
		if ( ! check_and_promote_exprs_to_type(constructor_list,
		                                       (*indices)[0]) )
			ExprError("inconsistent type in set constructor");
		}

	else if ( indices->length() > 1 )
		{
		// Check/promote each expression in composite index.
		loop_over_list(cle, i)
			{
			Expr* ce = cle[i];
			ListExpr* le = ce->AsListExpr();

			if ( ce->Tag() == EXPR_LIST &&
			     check_and_promote_exprs(le, type->AsTableType()->Indices()) )
				{
				if ( le != cle[i] )
					cle.replace(i, le);

				continue;
				}

			ExprError("inconsistent types in set constructor");
			}
		}
	}

Val* SetConstructorExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return 0;

	TableVal* aggr = new TableVal(type->AsTableType(), attrs);
	const expr_list& exprs = op->AsListExpr()->Exprs();

	for ( const auto& expr : exprs )
		{
		Val* element = expr->Eval(f);
		aggr->Assign(element, 0);
		Unref(element);
		}

	return aggr;
	}

Val* SetConstructorExpr::InitVal(const BroType* t, Val* aggr) const
	{
	if ( IsError() )
		return 0;

	const BroType* index_type = t->AsTableType()->Indices();
	TableType* tt = Type()->AsTableType();
	TableVal* tval = aggr ? aggr->AsTableVal() : new TableVal(tt, attrs);
	const expr_list& exprs = op->AsListExpr()->Exprs();

	for ( const auto& e : exprs )
		{
		Val* element = check_and_promote(e->Eval(0), index_type, 1);

		if ( ! element || ! tval->Assign(element, 0) )
			{
			Error(fmt("initialization type mismatch in set"), e);
			return 0;
			}

		Unref(element);
		}

	return tval;
	}

void SetConstructorExpr::ExprDescribe(ODesc* d) const
	{
	d->Add("set(");
	op->Describe(d);
	d->Add(")");
	}

VectorConstructorExpr::VectorConstructorExpr(ListExpr* constructor_list,
					     BroType* arg_type)
: UnaryExpr(EXPR_VECTOR_CONSTRUCTOR, constructor_list)
	{
	if ( IsError() )
		return;

	if ( arg_type )
		{
		if ( arg_type->Tag() != TYPE_VECTOR )
			{
			Error("bad vector constructor type", arg_type);
			SetError();
			return;
			}

		SetType(arg_type->Ref());
		}
	else
		{
		if ( constructor_list->Exprs().length() == 0 )
			{
			// vector().
			// By default, assign VOID type here. A vector with
			// void type set is seen as an unspecified vector.
			SetType(new ::VectorType(base_type(TYPE_VOID)));
			return;
			}

		BroType* t = merge_type_list(constructor_list);

		if ( t )
			{
			SetType(new VectorType(t->Ref()));
			Unref(t);
			}
		else
			{
			SetError();
			return;
			}
		}

	if ( ! check_and_promote_exprs_to_type(constructor_list,
					       type->AsVectorType()->YieldType()) )
		ExprError("inconsistent types in vector constructor");
	}

Val* VectorConstructorExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return 0;

	VectorVal* vec = new VectorVal(Type()->AsVectorType());
	const expr_list& exprs = op->AsListExpr()->Exprs();

	loop_over_list(exprs, i)
		{
		Expr* e = exprs[i];
		Val* v = e->Eval(f);
		if ( ! vec->Assign(i, v) )
			{
			RuntimeError(fmt("type mismatch at index %d", i));
			return 0;
			}
		}

	return vec;
	}

Val* VectorConstructorExpr::InitVal(const BroType* t, Val* aggr) const
	{
	if ( IsError() )
		return 0;

	VectorType* vt = Type()->AsVectorType();
	VectorVal* vec = aggr ? aggr->AsVectorVal() : new VectorVal(vt);
	const expr_list& exprs = op->AsListExpr()->Exprs();

	loop_over_list(exprs, i)
		{
		Expr* e = exprs[i];
		Val* v = check_and_promote(e->Eval(0), t->YieldType(), 1);

		if ( ! v || ! vec->Assign(i, v) )
			{
			Error(fmt("initialization type mismatch at index %d", i), e);
			if ( ! aggr )
				Unref(vec);
			return 0;
			}
		}

	return vec;
	}

void VectorConstructorExpr::ExprDescribe(ODesc* d) const
	{
	d->Add("vector(");
	op->Describe(d);
	d->Add(")");
	}

FieldAssignExpr::FieldAssignExpr(const char* arg_field_name, Expr* value)
: UnaryExpr(EXPR_FIELD_ASSIGN, value), field_name(arg_field_name)
	{
	op->Ref();
	SetType(value->Type()->Ref());
	}

void FieldAssignExpr::EvalIntoAggregate(const BroType* t, Val* aggr, Frame* f)
	const
	{
	if ( IsError() )
		return;

	RecordVal* rec = aggr->AsRecordVal();
	const RecordType* rt = t->AsRecordType();
	Val* v = op->Eval(f);

	if ( v )
		{
		int idx = rt->FieldOffset(field_name.c_str());

		if ( idx < 0 )
			reporter->InternalError("Missing record field: %s",
			                        field_name.c_str());

		rec->Assign(idx, v);
		}
	}

int FieldAssignExpr::IsRecordElement(TypeDecl* td) const
	{
	if ( td )
		{
		td->type = op->Type()->Ref();
		td->id = copy_string(field_name.c_str());
		}

	return 1;
	}

void FieldAssignExpr::ExprDescribe(ODesc* d) const
	{
	d->Add("$");
	d->Add(FieldName());
	d->Add("=");
	op->Describe(d);
	}

ArithCoerceExpr::ArithCoerceExpr(Expr* arg_op, TypeTag t)
: UnaryExpr(EXPR_ARITH_COERCE, arg_op)
	{
	if ( IsError() )
		return;

	TypeTag bt = op->Type()->Tag();
	TypeTag vbt = bt;

	if ( IsVector(bt) )
		{
		SetType(new VectorType(base_type(t)));
		vbt = op->Type()->AsVectorType()->YieldType()->Tag();
		}
	else
		SetType(base_type(t));

	if ( (bt == TYPE_ENUM) != (t == TYPE_ENUM) )
		ExprError("can't convert to/from enumerated type");

	else if ( ! IsArithmetic(t) && ! IsBool(t) &&
		  t != TYPE_TIME && t != TYPE_INTERVAL )
		ExprError("bad coercion");

	else if ( ! IsArithmetic(bt) && ! IsBool(bt) &&
		  ! IsArithmetic(vbt) && ! IsBool(vbt) )
		ExprError("bad coercion value");
	}

Val* ArithCoerceExpr::FoldSingleVal(Val* v, InternalTypeTag t) const
	{
	switch ( t ) {
	case TYPE_INTERNAL_DOUBLE:
		return new Val(v->CoerceToDouble(), TYPE_DOUBLE);

	case TYPE_INTERNAL_INT:
		return val_mgr->GetInt(v->CoerceToInt());

	case TYPE_INTERNAL_UNSIGNED:
		return val_mgr->GetCount(v->CoerceToUnsigned());

	default:
		RuntimeErrorWithCallStack("bad type in CoerceExpr::Fold");
		return 0;
	}
	}

Val* ArithCoerceExpr::Fold(Val* v) const
	{
	InternalTypeTag t = type->InternalType();

	if ( ! is_vector(v) )
		{
		// Our result type might be vector, in which case this
		// invocation is being done per-element rather than on
		// the whole vector.  Correct the type tag if necessary.
		if ( type->Tag() == TYPE_VECTOR )
			t = Type()->AsVectorType()->YieldType()->InternalType();
		return FoldSingleVal(v, t);
		}

	t = Type()->AsVectorType()->YieldType()->InternalType();

	VectorVal* vv = v->AsVectorVal();
	VectorVal* result = new VectorVal(Type()->AsVectorType());
	for ( unsigned int i = 0; i < vv->Size(); ++i )
		{
		Val* elt = vv->Lookup(i);
		if ( elt )
			result->Assign(i, FoldSingleVal(elt, t));
		else
			result->Assign(i, 0);
		}

	return result;
	}

RecordCoerceExpr::RecordCoerceExpr(Expr* op, RecordType* r)
: UnaryExpr(EXPR_RECORD_COERCE, op)
	{
	map_size = 0;
	map = 0;

	if ( IsError() )
		return;

	SetType(r->Ref());

	if ( Type()->Tag() != TYPE_RECORD )
		ExprError("coercion to non-record");

	else if ( op->Type()->Tag() != TYPE_RECORD )
		ExprError("coercion of non-record to record");

	else
		{
		RecordType* t_r = type->AsRecordType();
		RecordType* sub_r = op->Type()->AsRecordType();

		map_size = t_r->NumFields();
		map = new int[map_size];

		int i;
		for ( i = 0; i < map_size; ++i )
			map[i] = -1;	// -1 = field is not mapped

		for ( i = 0; i < sub_r->NumFields(); ++i )
			{
			int t_i = t_r->FieldOffset(sub_r->FieldName(i));
			if ( t_i < 0 )
				{
				ExprError(fmt("orphaned field \"%s\" in record coercion",
				              sub_r->FieldName(i)));
				break;
				}

			BroType* sub_t_i = sub_r->FieldType(i);
			BroType* sup_t_i = t_r->FieldType(t_i);

			if ( ! same_type(sup_t_i, sub_t_i) )
				{
				auto is_arithmetic_promotable = [](BroType* sup, BroType* sub) -> bool
					{
					auto sup_tag = sup->Tag();
					auto sub_tag = sub->Tag();

					if ( ! BothArithmetic(sup_tag, sub_tag) )
						return false;

					if ( sub_tag == TYPE_DOUBLE && IsIntegral(sup_tag) )
						return false;

					if ( sub_tag == TYPE_INT && sup_tag == TYPE_COUNT )
						return false;

					return true;
					};

				auto is_record_promotable = [](BroType* sup, BroType* sub) -> bool
					{
					if ( sup->Tag() != TYPE_RECORD )
						return false;

					if ( sub->Tag() != TYPE_RECORD )
						return false;

					return record_promotion_compatible(sup->AsRecordType(),
					                                   sub->AsRecordType());
					};

				if ( ! is_arithmetic_promotable(sup_t_i, sub_t_i) &&
				     ! is_record_promotable(sup_t_i, sub_t_i) )
					{
					string error_msg = fmt(
						"type clash for field \"%s\"", sub_r->FieldName(i));
					Error(error_msg.c_str(), sub_t_i);
					SetError();
					break;
					}
				}

			map[t_i] = i;
			}

		if ( IsError() )
			return;

		for ( i = 0; i < map_size; ++i )
			{
			if ( map[i] == -1 )
				{
				if ( ! t_r->FieldDecl(i)->FindAttr(ATTR_OPTIONAL) )
					{
					string error_msg = fmt(
						"non-optional field \"%s\" missing", t_r->FieldName(i));
					Error(error_msg.c_str());
					SetError();
					break;
					}
				}
			else if ( t_r->IsFieldDeprecated(i) )
				reporter->Warning("%s", t_r->GetFieldDeprecationWarning(i, false).c_str());
			}
		}
	}

RecordCoerceExpr::~RecordCoerceExpr()
	{
	delete [] map;
	}

Val* RecordCoerceExpr::InitVal(const BroType* t, Val* aggr) const
	{
	Val* v = Eval(0);

	if ( v )
		{
		RecordVal* rv = v->AsRecordVal();
		RecordVal* ar = rv->CoerceTo(t->AsRecordType(), aggr);

		if ( ar )
			{
			Unref(rv);
			return ar;
			}
		}

	Error("bad record initializer");
	return 0;
	}

Val* RecordCoerceExpr::Fold(Val* v) const
	{
	RecordVal* val = new RecordVal(Type()->AsRecordType());
	RecordVal* rv = v->AsRecordVal();

	for ( int i = 0; i < map_size; ++i )
		{
		if ( map[i] >= 0 )
			{
			Val* rhs = rv->Lookup(map[i]);
			if ( ! rhs )
				{
				const Attr* def = rv->Type()->AsRecordType()->FieldDecl(
					map[i])->FindAttr(ATTR_DEFAULT);

				if ( def )
					rhs = def->AttrExpr()->Eval(0);
				}

			if ( rhs )
				rhs = rhs->Ref();

			assert(rhs || Type()->AsRecordType()->FieldDecl(i)->FindAttr(ATTR_OPTIONAL));

			if ( ! rhs )
				{
				// Optional field is missing.
				val->Assign(i, 0);
				continue;
				}

			BroType* rhs_type = rhs->Type();
			RecordType* val_type = val->Type()->AsRecordType();
			BroType* field_type = val_type->FieldType(i);

			if ( rhs_type->Tag() == TYPE_RECORD &&
			     field_type->Tag() == TYPE_RECORD &&
			     ! same_type(rhs_type, field_type) )
				{
				Val* new_val = rhs->AsRecordVal()->CoerceTo(
				    field_type->AsRecordType());
				if ( new_val )
					{
					Unref(rhs);
					rhs = new_val;
					}
				}
			else if ( BothArithmetic(rhs_type->Tag(), field_type->Tag()) &&
			          ! same_type(rhs_type, field_type) )
				{
				if ( Val* new_val = check_and_promote(rhs, field_type, false, op->GetLocationInfo()) )
					{
					// Don't call unref here on rhs because check_and_promote already called it.
					rhs = new_val;
					}
				else
					{
					Unref(val);
					RuntimeError("Failed type conversion");
					}
				}

			val->Assign(i, rhs);
			}
		else
			{
			const Attr* def =
			     Type()->AsRecordType()->FieldDecl(i)->FindAttr(ATTR_DEFAULT);

			if ( def )
				{
				Val* def_val = def->AttrExpr()->Eval(0);
				BroType* def_type = def_val->Type();
				BroType* field_type = Type()->AsRecordType()->FieldType(i);

				if ( def_type->Tag() == TYPE_RECORD &&
				     field_type->Tag() == TYPE_RECORD &&
				     ! same_type(def_type, field_type) )
					{
					Val* tmp = def_val->AsRecordVal()->CoerceTo(
					        field_type->AsRecordType());

					if ( tmp )
						{
						Unref(def_val);
						def_val = tmp;
						}
					}

				val->Assign(i, def_val);
				}
			else
				val->Assign(i, 0);
			}
		}

	return val;
	}

TableCoerceExpr::TableCoerceExpr(Expr* op, TableType* r)
: UnaryExpr(EXPR_TABLE_COERCE, op)
	{
	if ( IsError() )
		return;

	SetType(r->Ref());

	if ( Type()->Tag() != TYPE_TABLE )
		ExprError("coercion to non-table");

	else if ( op->Type()->Tag() != TYPE_TABLE )
		ExprError("coercion of non-table/set to table/set");
	}


TableCoerceExpr::~TableCoerceExpr()
	{
	}

Val* TableCoerceExpr::Fold(Val* v) const
	{
	TableVal* tv = v->AsTableVal();

	if ( tv->Size() > 0 )
		RuntimeErrorWithCallStack("coercion of non-empty table/set");

	return new TableVal(Type()->AsTableType(), tv->Attrs());
	}

VectorCoerceExpr::VectorCoerceExpr(Expr* op, VectorType* v)
: UnaryExpr(EXPR_VECTOR_COERCE, op)
	{
	if ( IsError() )
		return;

	SetType(v->Ref());

	if ( Type()->Tag() != TYPE_VECTOR )
		ExprError("coercion to non-vector");

	else if ( op->Type()->Tag() != TYPE_VECTOR )
		ExprError("coercion of non-vector to vector");
	}


VectorCoerceExpr::~VectorCoerceExpr()
	{
	}

Val* VectorCoerceExpr::Fold(Val* v) const
	{
	VectorVal* vv = v->AsVectorVal();

	if ( vv->Size() > 0 )
		RuntimeErrorWithCallStack("coercion of non-empty vector");

	return new VectorVal(Type()->Ref()->AsVectorType());
	}

FlattenExpr::FlattenExpr(Expr* arg_op)
: UnaryExpr(EXPR_FLATTEN, arg_op)
	{
	if ( IsError() )
		return;

	BroType* t = op->Type();
	if ( t->Tag() != TYPE_RECORD )
		Internal("bad type in FlattenExpr::FlattenExpr");

	RecordType* rt = t->AsRecordType();
	num_fields = rt->NumFields();

	TypeList* tl = new TypeList();
	for ( int i = 0; i < num_fields; ++i )
		tl->Append(rt->FieldType(i)->Ref());

	Unref(rt);
	SetType(tl);
	}

Val* FlattenExpr::Fold(Val* v) const
	{
	RecordVal* rv = v->AsRecordVal();
	ListVal* l = new ListVal(TYPE_ANY);

	for ( int i = 0; i < num_fields; ++i )
		{
		Val* fv = rv->Lookup(i);

		if ( fv )
			{
			l->Append(fv->Ref());
			continue;
			}

		const RecordType* rv_t = rv->Type()->AsRecordType();
		const Attr* fa = rv_t->FieldDecl(i)->FindAttr(ATTR_DEFAULT);
		if ( fa )
			l->Append(fa->AttrExpr()->Eval(0));

		else
			RuntimeError("missing field value");
		}

	return l;
	}

ScheduleTimer::ScheduleTimer(EventHandlerPtr arg_event, val_list* arg_args,
				double t, TimerMgr* arg_tmgr)
	: Timer(t, TIMER_SCHEDULE),
	  event(arg_event),
	  args(std::move(*arg_args)),
	  tmgr(arg_tmgr)
	{
	delete arg_args;
	}

ScheduleTimer::~ScheduleTimer()
	{
	}

void ScheduleTimer::Dispatch(double /* t */, int /* is_expire */)
	{
	mgr.QueueEvent(event, std::move(args), SOURCE_LOCAL, 0, tmgr);
	}

ScheduleExpr::ScheduleExpr(Expr* arg_when, EventExpr* arg_event)
: Expr(EXPR_SCHEDULE)
	{
	when = arg_when;
	event = arg_event;

	if ( IsError() || when->IsError() || event->IsError() )
		return;

	TypeTag bt = when->Type()->Tag();
	if ( bt != TYPE_TIME && bt != TYPE_INTERVAL )
		ExprError("schedule expression requires a time or time interval");
	else
		SetType(base_type(TYPE_TIMER));
	}

ScheduleExpr::~ScheduleExpr()
	{
	Unref(when);
	Unref(event);
	}

int ScheduleExpr::IsPure() const
	{
	return 0;
	}

Val* ScheduleExpr::Eval(Frame* f) const
	{
	if ( terminating )
		return 0;

	Val* when_val = when->Eval(f);
	if ( ! when_val )
		return 0;

	double dt = when_val->InternalDouble();
	if ( when->Type()->Tag() == TYPE_INTERVAL )
		dt += network_time;

	val_list* args = eval_list(f, event->Args());

	if ( args )
		{
		TimerMgr* tmgr = mgr.CurrentTimerMgr();

		if ( ! tmgr )
			tmgr = timer_mgr;

		tmgr->Add(new ScheduleTimer(event->Handler(), args, dt, tmgr));
		}

	Unref(when_val);

	return 0;
	}

TraversalCode ScheduleExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = when->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = event->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void ScheduleExpr::ExprDescribe(ODesc* d) const
	{
	if ( d->IsReadable() )
		d->AddSP("schedule");

	when->Describe(d);
	d->SP();

	if ( d->IsReadable() )
		{
		d->Add("{");
		d->PushIndent();
		event->Describe(d);
		d->PopIndent();
		d->Add("}");
		}
	else
		event->Describe(d);
	}

InExpr::InExpr(Expr* arg_op1, Expr* arg_op2)
: BinaryExpr(EXPR_IN, arg_op1, arg_op2)
	{
	if ( IsError() )
		return;

	if ( op1->Type()->Tag() == TYPE_PATTERN )
		{
		if ( op2->Type()->Tag() != TYPE_STRING )
			{
			op2->Type()->Error("pattern requires string index", op1);
			SetError();
			}
		else
			SetType(base_type(TYPE_BOOL));
		}

	else if ( op1->Type()->Tag() == TYPE_RECORD )
		{
		if ( op2->Type()->Tag() != TYPE_TABLE )
			{
			op2->Type()->Error("table/set required");
			SetError();
			}

		else
			{
			const BroType* t1 = op1->Type();
			const TypeList* it =
				op2->Type()->AsTableType()->Indices();

			if ( ! same_type(t1, it) )
				{
				t1->Error("indexing mismatch", op2->Type());
				SetError();
				}
			else
				SetType(base_type(TYPE_BOOL));
			}
		}

	else if ( op1->Type()->Tag() == TYPE_STRING &&
		  op2->Type()->Tag() == TYPE_STRING )
		SetType(base_type(TYPE_BOOL));

	else
		{
		// Check for:	<addr> in <subnet>
		//		<addr> in set[subnet]
		//		<addr> in table[subnet] of ...
		if ( op1->Type()->Tag() == TYPE_ADDR )
			{
			if ( op2->Type()->Tag() == TYPE_SUBNET )
				{
				SetType(base_type(TYPE_BOOL));
				return;
				}

			if ( op2->Type()->Tag() == TYPE_TABLE &&
			     op2->Type()->AsTableType()->IsSubNetIndex() )
				{
				SetType(base_type(TYPE_BOOL));
				return;
				}
			}

		if ( op1->Tag() != EXPR_LIST )
			op1 = new ListExpr(op1);

		ListExpr* lop1 = op1->AsListExpr();

		if ( ! op2->Type()->MatchesIndex(lop1) )
			SetError("not an index type");
		else
			{
			op1 = lop1;
			SetType(base_type(TYPE_BOOL));
			}
		}
	}

Val* InExpr::Fold(Val* v1, Val* v2) const
	{
	if ( v1->Type()->Tag() == TYPE_PATTERN )
		{
		RE_Matcher* re = v1->AsPattern();
		const BroString* s = v2->AsString();
		return val_mgr->GetBool(re->MatchAnywhere(s) != 0);
		}

	if ( v2->Type()->Tag() == TYPE_STRING )
		{
		const BroString* s1 = v1->AsString();
		const BroString* s2 = v2->AsString();

		// Could do better here e.g. Boyer-Moore if done repeatedly.
		return val_mgr->GetBool(strstr_n(s2->Len(), s2->Bytes(), s1->Len(), reinterpret_cast<const unsigned char*>(s1->CheckString())) != -1);
		}

	if ( v1->Type()->Tag() == TYPE_ADDR &&
	     v2->Type()->Tag() == TYPE_SUBNET )
		return val_mgr->GetBool(v2->AsSubNetVal()->Contains(v1->AsAddr()));

	Val* res;

	if ( is_vector(v2) )
		res = v2->AsVectorVal()->Lookup(v1);
	else
		res = v2->AsTableVal()->Lookup(v1, false);

	if ( res )
		return val_mgr->GetBool(1);
	else
		return val_mgr->GetBool(0);
	}

CallExpr::CallExpr(Expr* arg_func, ListExpr* arg_args, bool in_hook)
: Expr(EXPR_CALL)
	{
	func = arg_func;
	args = arg_args;

	if ( func->IsError() || args->IsError() )
		{
		SetError();
		return;
		}

	BroType* func_type = func->Type();
	if ( ! IsFunc(func_type->Tag()) )
		{
		func->Error("not a function");
		SetError();
		return;
		}

	if ( func_type->AsFuncType()->Flavor() == FUNC_FLAVOR_HOOK && ! in_hook )
		{
		func->Error("hook cannot be called directly, use hook operator");
		SetError();
		return;
		}

	if ( ! func_type->MatchesIndex(args) )
		SetError("argument type mismatch in function call");
	else
		{
		BroType* yield = func_type->YieldType();

		if ( ! yield )
			{
			switch ( func_type->AsFuncType()->Flavor() ) {

			case FUNC_FLAVOR_FUNCTION:
				Error("function has no yield type");
				SetError();
				break;

			case FUNC_FLAVOR_EVENT:
				Error("event called in expression, use event statement instead");
				SetError();
				break;

			case FUNC_FLAVOR_HOOK:
				Error("hook has no yield type");
				SetError();
				break;

			default:
				Error("invalid function flavor");
				SetError();
				break;
			}
			}
		else
			SetType(yield->Ref());

		// Check for call to built-ins that can be statically
		// analyzed.
		Val* func_val;
		if ( func->Tag() == EXPR_NAME &&
		     // This is cheating, but without it processing gets
		     // quite confused regarding "value used but not set"
		     // run-time errors when we apply this analysis during
		     // parsing.  Really we should instead do it after we've
		     // parsed the entire set of scripts.
		     streq(((NameExpr*) func)->Id()->Name(), "fmt") &&
		     // The following is needed because fmt might not yet
		     // be bound as a name.
		     did_builtin_init &&
		     (func_val = func->Eval(0)) )
			{
			::Func* f = func_val->AsFunc();
			if ( f->GetKind() == Func::BUILTIN_FUNC &&
			     ! check_built_in_call((BuiltinFunc*) f, this) )
				SetError();
			}
		}
	}

CallExpr::~CallExpr()
	{
	Unref(func);
	Unref(args);
	}

int CallExpr::IsPure() const
	{
	if ( IsError() )
		return 1;

	if ( ! func->IsPure() )
		return 0;

	Val* func_val = func->Eval(0);
	if ( ! func_val )
		return 0;

	::Func* f = func_val->AsFunc();

	// Only recurse for built-in functions, as recursing on script
	// functions can lead to infinite recursion if the function being
	// called here happens to be recursive (either directly
	// or indirectly).
	int pure = 0;
	if ( f->GetKind() == Func::BUILTIN_FUNC )
		pure = f->IsPure() && args->IsPure();
	Unref(func_val);

	return pure;
	}

Val* CallExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return 0;

	// If we are inside a trigger condition, we may have already been
	// called, delayed, and then produced a result which is now cached.
	// Check for that.
	if ( f )
		{
		Trigger* trigger = f->GetTrigger();

		if ( trigger )
			{
			Val* v = trigger->Lookup(this);
			if ( v )
				{
				DBG_LOG(DBG_NOTIFIERS,
					"%s: provides cached function result",
					trigger->Name());
				return v->Ref();
				}
			}
		}

	Val* ret = 0;
	Val* func_val = func->Eval(f);
	val_list* v = eval_list(f, args);

	if ( func_val && v )
		{
		const ::Func* func = func_val->AsFunc();
		const CallExpr* current_call = f ? f->GetCall() : 0;

		if ( f )
			f->SetCall(this);

		ret = func->Call(v, f);

		if ( f )
			f->SetCall(current_call);

		// Don't Unref() the arguments, as Func::Call already did that.
		delete v;
		}
	else
		delete_vals(v);

	Unref(func_val);

	return ret;
	}

TraversalCode CallExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = func->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = args->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void CallExpr::ExprDescribe(ODesc* d) const
	{
	func->Describe(d);
	if ( d->IsReadable() || d->IsPortable() )
		{
		d->Add("(");
		args->Describe(d);
		d->Add(")");
		}
	else
		args->Describe(d);
	}

LambdaExpr::LambdaExpr(std::unique_ptr<function_ingredients> arg_ing,
		       id_list arg_outer_ids) : Expr(EXPR_LAMBDA)
	{
	ingredients = std::move(arg_ing);
	outer_ids = std::move(arg_outer_ids);

	SetType(ingredients->id->Type()->Ref());

	// Install a dummy version of the function globally for use only
	// when broker provides a closure.
	BroFunc* dummy_func = new BroFunc(
		ingredients->id,
		ingredients->body,
		ingredients->inits,
		ingredients->frame_size,
		ingredients->priority);

	dummy_func->SetOuterIDs(outer_ids);

	// Get the body's "string" representation.
	ODesc d;
	dummy_func->Describe(&d);

	for ( ; ; )
		{
		uint64_t h[2];
		internal_md5(d.Bytes(), d.Len(), reinterpret_cast<unsigned char*>(h));

		my_name = "lambda_<" + std::to_string(h[0]) + ">";
		auto fullname = make_full_var_name(current_module.data(), my_name.data());
		auto id = global_scope()->Lookup(fullname.data());

		if ( id )
			// Just try again to make a unique lambda name.  If two peer
			// processes need to agree on the same lambda name, this assumes
			// they're loading the same scripts and thus have the same hash
			// collisions.
			d.Add(" ");
		else
			break;
		}

	// Install that in the global_scope
	ID* id = install_ID(my_name.c_str(), current_module.c_str(), true, false);

	// Update lamb's name
	dummy_func->SetName(my_name.c_str());

	Val* v = new Val(dummy_func);
	id->SetVal(v); // id will unref v when its done.
	id->SetType(ingredients->id->Type()->Ref());
	id->SetConst();
	}

Val* LambdaExpr::Eval(Frame* f) const
	{
	BroFunc* lamb = new BroFunc(
		ingredients->id,
		ingredients->body,
		ingredients->inits,
		ingredients->frame_size,
		ingredients->priority);

	lamb->AddClosure(outer_ids, f);

	// Set name to corresponding dummy func.
	// Allows for lookups by the receiver.
	lamb->SetName(my_name.c_str());

	return new Val(lamb);
	}

void LambdaExpr::ExprDescribe(ODesc* d) const
	{
	d->Add(expr_name(Tag()));
	ingredients->body->Describe(d);
	}

TraversalCode LambdaExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = ingredients->body->Traverse(cb);
	HANDLE_TC_STMT_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

EventExpr::EventExpr(const char* arg_name, ListExpr* arg_args)
: Expr(EXPR_EVENT)
	{
	name = arg_name;
	args = arg_args;

	EventHandler* h = event_registry->Lookup(name.c_str());
	if ( ! h )
		{
		h = new EventHandler(name.c_str());
		event_registry->Register(h);
		}

	h->SetUsed();

	handler = h;

	if ( args->IsError() )
		{
		SetError();
		return;
		}

	FuncType* func_type = h->FType();
	if ( ! func_type )
		{
		Error("not an event");
		SetError();
		return;
		}

	if ( ! func_type->MatchesIndex(args) )
		SetError("argument type mismatch in event invocation");
	else
		{
		if ( func_type->YieldType() )
			{
			Error("function invoked as an event");
			SetError();
			}
		}
	}

EventExpr::~EventExpr()
	{
	Unref(args);
	}

Val* EventExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return 0;

	val_list* v = eval_list(f, args);
	mgr.QueueEvent(handler, std::move(*v));
	delete v;

	return 0;
	}

TraversalCode EventExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	tc = args->Traverse(cb);
	HANDLE_TC_EXPR_PRE(tc);

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

void EventExpr::ExprDescribe(ODesc* d) const
	{
	d->Add(name.c_str());
	if ( d->IsReadable() || d->IsPortable() )
		{
		d->Add("(");
		args->Describe(d);
		d->Add(")");
		}
	else
		args->Describe(d);
	}

ListExpr::ListExpr() : Expr(EXPR_LIST)
	{
	SetType(new TypeList());
	}

ListExpr::ListExpr(Expr* e) : Expr(EXPR_LIST)
	{
	SetType(new TypeList());
	Append(e);
	}

ListExpr::~ListExpr()
	{
	for ( const auto& expr: exprs )
		Unref(expr);
	}

void ListExpr::Append(Expr* e)
	{
	exprs.push_back(e);
	((TypeList*) type)->Append(e->Type()->Ref());
	}

int ListExpr::IsPure() const
	{
	for ( const auto& expr : exprs )
		if ( ! expr->IsPure() )
			return 0;

	return 1;
	}

int ListExpr::AllConst() const
	{
	for ( const auto& expr : exprs )
		if ( ! expr->IsConst() )
			return 0;

	return 1;
	}

Val* ListExpr::Eval(Frame* f) const
	{
	ListVal* v = new ListVal(TYPE_ANY);

	for ( const auto& expr : exprs )
		{
		Val* ev = expr->Eval(f);
		if ( ! ev )
			{
			RuntimeError("uninitialized list value");
			Unref(v);
			return 0;
			}

		v->Append(ev);
		}

	return v;
	}

BroType* ListExpr::InitType() const
	{
	if ( exprs.length() == 0 )
		{
		Error("empty list in untyped initialization");
		return 0;
		}

	if ( exprs[0]->IsRecordElement(0) )
		{
		type_decl_list* types = new type_decl_list(exprs.length());
		for ( const auto& expr : exprs )
			{
			TypeDecl* td = new TypeDecl(0, 0);
			if ( ! expr->IsRecordElement(td) )
				{
				expr->Error("record element expected");
				delete td;
				delete types;
				return 0;
				}

			types->push_back(td);
			}


		return new RecordType(types);
		}

	else
		{
		TypeList* tl = new TypeList();
		for ( const auto& e : exprs )
			{
			BroType* ti = e->Type();

			// Collapse any embedded sets or lists.
			if ( ti->IsSet() || ti->Tag() == TYPE_LIST )
				{
				TypeList* til = ti->IsSet() ?
					ti->AsSetType()->Indices() :
					ti->AsTypeList();

				if ( ! til->IsPure() ||
				     ! til->AllMatch(til->PureType(), 1) )
					tl->Append(til->Ref());
				else
					tl->Append(til->PureType()->Ref());
				}
			else
				tl->Append(ti->Ref());
			}

		return tl;
		}
	}

Val* ListExpr::InitVal(const BroType* t, Val* aggr) const
	{
	// While fairly similar to the EvalIntoAggregate() code,
	// we keep this separate since it also deals with initialization
	// idioms such as embedded aggregates and cross-product
	// expansion.
	if ( IsError() )
		return 0;

	// Check whether each element of this list itself matches t,
	// in which case we should expand as a ListVal.
	if ( ! aggr && type->AsTypeList()->AllMatch(t, 1) )
		{
		ListVal* v = new ListVal(TYPE_ANY);

		const type_list* tl = type->AsTypeList()->Types();
		if ( exprs.length() != tl->length() )
			{
			Error("index mismatch", t);
			Unref(v);
			return 0;
			}

		loop_over_list(exprs, i)
			{
			Val* vi = exprs[i]->InitVal((*tl)[i], 0);
			if ( ! vi )
				{
				Unref(v);
				return 0;
				}

			v->Append(vi);
			}
		return v;
		}

	if ( t->Tag() == TYPE_LIST )
		{
		if ( aggr )
			{
			Error("bad use of list in initialization", t);
			return 0;
			}

		const type_list* tl = t->AsTypeList()->Types();
		if ( exprs.length() != tl->length() )
			{
			Error("index mismatch", t);
			return 0;
			}

		ListVal* v = new ListVal(TYPE_ANY);
		loop_over_list(exprs, i)
			{
			Val* vi = exprs[i]->InitVal((*tl)[i], 0);
			if ( ! vi )
				{
				Unref(v);
				return 0;
				}
			v->Append(vi);
			}
		return v;
		}

	if ( t->Tag() != TYPE_RECORD && t->Tag() != TYPE_TABLE &&
	     t->Tag() != TYPE_VECTOR )
		{
		if ( exprs.length() == 1 )
			// Allow "global x:int = { 5 }"
			return exprs[0]->InitVal(t, aggr);
		else
			{
			Error("aggregate initializer for scalar type", t);
			return 0;
			}
		}

	if ( ! aggr )
		Internal("missing aggregate in ListExpr::InitVal");

	if ( t->IsSet() )
		return AddSetInit(t, aggr);

	if ( t->Tag() == TYPE_VECTOR )
		{
		// v: vector = [10, 20, 30];
		VectorVal* vec = aggr->AsVectorVal();

		loop_over_list(exprs, i)
			{
			Expr* e = exprs[i];
			check_and_promote_expr(e, vec->Type()->AsVectorType()->YieldType());
			Val* v = e->Eval(0);
			if ( ! vec->Assign(i, v) )
				{
				e->Error(fmt("type mismatch at index %d", i));
				return 0;
				}
			}

		return aggr;
		}

	// If we got this far, then it's either a table or record
	// initialization.  Both of those involve AssignExpr's, which
	// know how to add themselves to a table or record.  Another
	// possibility is an expression that evaluates itself to a
	// table, which we can then add to the aggregate.
	for ( const auto& e : exprs )
		{
		if ( e->Tag() == EXPR_ASSIGN || e->Tag() == EXPR_FIELD_ASSIGN )
			{
			if ( ! e->InitVal(t, aggr) )
				return 0;
			}
		else
			{
			if ( t->Tag() == TYPE_RECORD )
				{
				e->Error("bad record initializer", t);
				return 0;
				}

			Val* v = e->Eval(0);
			if ( ! same_type(v->Type(), t) )
				{
				v->Type()->Error("type clash in table initializer", t);
				return 0;
				}

			if ( ! v->AsTableVal()->AddTo(aggr->AsTableVal(), 1) )
				return 0;
			}
		}

	return aggr;
	}

Val* ListExpr::AddSetInit(const BroType* t, Val* aggr) const
	{
	if ( aggr->Type()->Tag() != TYPE_TABLE )
		Internal("bad aggregate in ListExpr::InitVal");

	TableVal* tv = aggr->AsTableVal();
	const TableType* tt = tv->Type()->AsTableType();
	const TypeList* it = tt->Indices();

	for ( const auto& expr : exprs )
		{
		Val* element;

		if ( expr->Type()->IsSet() )
			// A set to flatten.
			element = expr->Eval(0);
		else if ( expr->Type()->Tag() == TYPE_LIST )
			element = expr->InitVal(it, 0);
		else
			element = expr->InitVal((*it->Types())[0], 0);

		if ( ! element )
			return 0;

		if ( element->Type()->IsSet() )
			{
			if ( ! same_type(element->Type(), t) )
				{
				element->Error("type clash in set initializer", t);
				return 0;
				}

			if ( ! element->AsTableVal()->AddTo(tv, 1) )
				return 0;

			continue;
			}

		if ( expr->Type()->Tag() == TYPE_LIST )
			element = check_and_promote(element, it, 1);
		else
			element = check_and_promote(element, (*it->Types())[0], 1);

		if ( ! element )
			return 0;

		if ( ! tv->ExpandAndInit(element, 0) )
			{
			Unref(element);
			Unref(tv);
			return 0;
			}

		Unref(element);
		}

	return tv;
	}

void ListExpr::ExprDescribe(ODesc* d) const
	{
	d->AddCount(exprs.length());

	loop_over_list(exprs, i)
		{
		if ( (d->IsReadable() || d->IsPortable()) && i > 0 )
			d->Add(", ");

		exprs[i]->Describe(d);
		}
	}

Expr* ListExpr::MakeLvalue()
	{
	for ( const auto & expr : exprs )
		if ( expr->Tag() != EXPR_NAME )
			ExprError("can only assign to list of identifiers");

	return new RefExpr(this);
	}

void ListExpr::Assign(Frame* f, Val* v)
	{
	ListVal* lv = v->AsListVal();

	if ( exprs.length() != lv->Vals()->length() )
		RuntimeError("mismatch in list lengths");

	loop_over_list(exprs, i)
		exprs[i]->Assign(f, (*lv->Vals())[i]->Ref());

	Unref(lv);
	}

TraversalCode ListExpr::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreExpr(this);
	HANDLE_TC_EXPR_PRE(tc);

	for ( const auto& expr : exprs )
		{
		tc = expr->Traverse(cb);
		HANDLE_TC_EXPR_PRE(tc);
		}

	tc = cb->PostExpr(this);
	HANDLE_TC_EXPR_POST(tc);
	}

RecordAssignExpr::RecordAssignExpr(Expr* record, Expr* init_list, int is_init)
	{
	const expr_list& inits = init_list->AsListExpr()->Exprs();

	RecordType* lhs = record->Type()->AsRecordType();

	// The inits have two forms:
	// 1) other records -- use all matching field names+types
	// 2) a string indicating the field name, then (as the next element)
	//    the value to use for that field.

	for ( const auto& init : inits )
		{
		if ( init->Type()->Tag() == TYPE_RECORD )
			{
			RecordType* t = init->Type()->AsRecordType();

			for ( int j = 0; j < t->NumFields(); ++j )
				{
				const char* field_name = t->FieldName(j);
				int field = lhs->FieldOffset(field_name);

				if ( field >= 0 &&
				     same_type(lhs->FieldType(field), t->FieldType(j)) )
					{
					FieldExpr* fe_lhs = new FieldExpr(record, field_name);
					FieldExpr* fe_rhs = new FieldExpr(init, field_name);
					Append(get_assign_expr(fe_lhs->Ref(), fe_rhs->Ref(), is_init));
					}
				}
			}

		else if ( init->Tag() == EXPR_FIELD_ASSIGN )
			{
			FieldAssignExpr* rf = (FieldAssignExpr*) init;
			rf->Ref();

			const char* field_name = ""; // rf->FieldName();
			if ( lhs->HasField(field_name) )
				{
				FieldExpr* fe_lhs = new FieldExpr(record, field_name);
				Expr* fe_rhs = rf->Op();
				Append(get_assign_expr(fe_lhs->Ref(), fe_rhs, is_init));
				}
			else
				{
				string s = "No such field '";
				s += field_name;
				s += "'";
				init_list->SetError(s.c_str());
				}
			}

		else
			{
			init_list->SetError("bad record initializer");
			return;
			}
		}
	}

CastExpr::CastExpr(Expr* arg_op, BroType* t) : UnaryExpr(EXPR_CAST, arg_op)
	{
	auto stype = Op()->Type();

	::Ref(t);
	SetType(t);

	if ( ! can_cast_value_to_type(stype, t) )
		ExprError("cast not supported");
	}

Val* CastExpr::Eval(Frame* f) const
	{
	if ( IsError() )
		return 0;

	Val* v = op->Eval(f);

	if ( ! v )
		return 0;

	Val* nv = cast_value_to_type(v, Type());

	if ( nv )
		{
		Unref(v);
		return nv;
		}

	ODesc d;
	d.Add("invalid cast of value with type '");
	v->Type()->Describe(&d);
	d.Add("' to type '");
	Type()->Describe(&d);
	d.Add("'");

	if ( same_type(v->Type(), bro_broker::DataVal::ScriptDataType()) &&
		 ! v->AsRecordVal()->Lookup(0) )
		d.Add(" (nil $data field)");

	Unref(v);
	RuntimeError(d.Description());
	return 0;  // not reached.
	}

void CastExpr::ExprDescribe(ODesc* d) const
	{
	Op()->Describe(d);
	d->Add(" as ");
	Type()->Describe(d);
	}

IsExpr::IsExpr(Expr* arg_op, BroType* arg_t) : UnaryExpr(EXPR_IS, arg_op)
	{
	t = arg_t;
	::Ref(t);

	SetType(base_type(TYPE_BOOL));
	}

IsExpr::~IsExpr()
	{
	Unref(t);
	}

Val* IsExpr::Fold(Val* v) const
	{
	if ( IsError() )
		return 0;

	if ( can_cast_value_to_type(v, t) )
		return val_mgr->GetBool(1);
	else
		return val_mgr->GetBool(0);
	}

void IsExpr::ExprDescribe(ODesc* d) const
	{
	Op()->Describe(d);
	d->Add(" is ");
	t->Describe(d);
	}

Expr* get_assign_expr(Expr* op1, Expr* op2, int is_init)
	{
	if ( op1->Type()->Tag() == TYPE_RECORD &&
	     op2->Type()->Tag() == TYPE_LIST )
		return new RecordAssignExpr(op1, op2, is_init);
	else if ( op1->Tag() == EXPR_INDEX && op1->AsIndexExpr()->IsSlice() )
		return new IndexSliceAssignExpr(op1, op2, is_init);
	else
		return new AssignExpr(op1, op2, is_init);
	}

int check_and_promote_expr(Expr*& e, BroType* t)
	{
	BroType* et = e->Type();
	TypeTag e_tag = et->Tag();
	TypeTag t_tag = t->Tag();

	if ( t->Tag() == TYPE_ANY )
		return 1;

	if ( EitherArithmetic(t_tag, e_tag) )
		{
		if ( e_tag == t_tag )
			return 1;

		if ( ! BothArithmetic(t_tag, e_tag) )
			{
			t->Error("arithmetic mixed with non-arithmetic", e);
			return 0;
			}

		TypeTag mt = max_type(t_tag, e_tag);
		if ( mt != t_tag )
			{
			t->Error("over-promotion of arithmetic value", e);
			return 0;
			}

		e = new ArithCoerceExpr(e, t_tag);
		return 1;
		}

	if ( t->Tag() == TYPE_RECORD && et->Tag() == TYPE_RECORD )
		{
		RecordType* t_r = t->AsRecordType();
		RecordType* et_r = et->AsRecordType();

		if ( same_type(t, et) )
			{
			// Make sure the attributes match as well.
			for ( int i = 0; i < t_r->NumFields(); ++i )
				{
				const TypeDecl* td1 = t_r->FieldDecl(i);
				const TypeDecl* td2 = et_r->FieldDecl(i);

				if ( same_attrs(td1->attrs, td2->attrs) )
					// Everything matches perfectly.
					return 1;
				}
			}

		if ( record_promotion_compatible(t_r, et_r) )
			{
			e = new RecordCoerceExpr(e, t_r);
			return 1;
			}

		t->Error("incompatible record types", e);
		return 0;
		}


	if ( ! same_type(t, et) )
		{
		if ( t->Tag() == TYPE_TABLE && et->Tag() == TYPE_TABLE &&
			  et->AsTableType()->IsUnspecifiedTable() )
			{
			e = new TableCoerceExpr(e, t->AsTableType());
			return 1;
			}

		if ( t->Tag() == TYPE_VECTOR && et->Tag() == TYPE_VECTOR &&
		     et->AsVectorType()->IsUnspecifiedVector() )
			{
			e = new VectorCoerceExpr(e, t->AsVectorType());
			return 1;
			}

		t->Error("type clash", e);
		return 0;
		}

	return 1;
	}

int check_and_promote_exprs(ListExpr*& elements, TypeList* types)
	{
	expr_list& el = elements->Exprs();
	const type_list* tl = types->Types();

	if ( tl->length() == 1 && (*tl)[0]->Tag() == TYPE_ANY )
		return 1;

	if ( el.length() != tl->length() )
		{
		types->Error("indexing mismatch", elements);
		return 0;
		}

	loop_over_list(el, i)
		{
		Expr* e = el[i];
		if ( ! check_and_promote_expr(e, (*tl)[i]) )
			{
			e->Error("type mismatch", (*tl)[i]);
			return 0;
			}

		if ( e != el[i] )
			el.replace(i, e);
		}

	return 1;
	}

int check_and_promote_args(ListExpr*& args, RecordType* types)
	{
	expr_list& el = args->Exprs();
	int ntypes = types->NumFields();

	// give variadic BIFs automatic pass
	if ( ntypes == 1 && types->FieldDecl(0)->type->Tag() == TYPE_ANY )
		return 1;

	if ( el.length() < ntypes )
		{
		expr_list def_elements;

		// Start from rightmost parameter, work backward to fill in missing
		// arguments using &default expressions.
		for ( int i = ntypes - 1; i >= el.length(); --i )
			{
			TypeDecl* td = types->FieldDecl(i);
			Attr* def_attr = td->attrs ? td->attrs->FindAttr(ATTR_DEFAULT) : 0;

			if ( ! def_attr )
				{
				types->Error("parameter mismatch", args);
				return 0;
				}

			def_elements.push_front(def_attr->AttrExpr());
			}

		for ( const auto& elem : def_elements )
			el.push_back(elem->Ref());
		}

	TypeList* tl = new TypeList();

	for ( int i = 0; i < types->NumFields(); ++i )
		tl->Append(types->FieldType(i)->Ref());

	int rval = check_and_promote_exprs(args, tl);
	Unref(tl);

	return rval;
	}

int check_and_promote_exprs_to_type(ListExpr*& elements, BroType* type)
	{
	expr_list& el = elements->Exprs();

	if ( type->Tag() == TYPE_ANY )
		return 1;

	loop_over_list(el, i)
		{
		Expr* e = el[i];
		if ( ! check_and_promote_expr(e, type) )
			{
			e->Error("type mismatch", type);
			return 0;
			}

		if ( e != el[i] )
			el.replace(i, e);
		}

	return 1;
	}

val_list* eval_list(Frame* f, const ListExpr* l)
	{
	const expr_list& e = l->Exprs();
	val_list* v = new val_list(e.length());

	bool success = true;
	for ( const auto& expr : e )
		{
		Val* ev = expr->Eval(f);
		if ( ! ev )
			{
			success = false;
			break;
			}
		v->push_back(ev);
		}

	if ( ! success )
		{ // Failure.
		for ( const auto& val : *v )
			Unref(val);
		delete v;
		return 0;
		}

	else
		return v;
	}

int expr_greater(const Expr* e1, const Expr* e2)
	{
	return int(e1->Tag()) > int(e2->Tag());
	}
